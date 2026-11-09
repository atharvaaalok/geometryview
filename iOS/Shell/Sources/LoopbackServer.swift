import Foundation
import Network

/// A minimal HTTP server on the phone's own loopback address, serving the shell's web files.
///
/// WebKit grants cross-origin isolation, which WebAssembly threads need, only to pages whose
/// responses carry the isolation headers over HTTP: the geo:// scheme handler's are not honoured.
/// 127.0.0.1 is a secure context, never leaves the device, and needs no network permission.
/// Each connection answers one GET and closes; files stream in 2 MB chunks. Everything runs on one
/// serial queue, which is what makes it safe to share.
///
/// File bodies are sent chunked, with no Content-Length. WebKit holds back a body whose length it
/// knows, handing it to the page some 80 ms at a time, so a file the loopback sends within that
/// time reached the page whole and late: a 256 MB cache file, meant to stream into GPU buffers,
/// sat in the page's memory first. A chunked body is handed over as it arrives. The size goes in
/// Geometry-Size instead, for the viewer to bound what a file's header may ask for.
final class LoopbackServer: @unchecked Sendable {
    private let files: WebFiles
    private let listener: NWListener
    private let queue = DispatchQueue(label: "geo.loopback")

    init(files: WebFiles) throws {
        self.files = files
        // Small writes go out at once, rather than waiting on the reader's delayed acknowledgement.
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
        listener = try NWListener(using: parameters)
    }

    /// Starts listening and reports the port, once, when it is known.
    func start(ready: @escaping @Sendable (UInt16) -> Void) {
        listener.stateUpdateHandler = { [listener] state in
            if case .ready = state, let port = listener.port {
                ready(port.rawValue)
            }
        }
        listener.newConnectionHandler = { [self] connection in
            connection.start(queue: queue)
            receive(connection, buffer: Data())
        }
        listener.start(queue: queue)
    }

    // Reads until the end of the request's headers, then answers.
    private func receive(_ connection: NWConnection, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) {
            [self] data, _, complete, error in
            var request = buffer
            if let data {
                request.append(data)
            }
            if let end = request.range(of: Data("\r\n\r\n".utf8)) {
                answer(connection, head: request[..<end.lowerBound])
            } else if complete || error != nil || request.count > 64 * 1024 {
                connection.cancel()
            } else {
                receive(connection, buffer: request)
            }
        }
    }

    private func answer(_ connection: NWConnection, head: Data) {
        let line = String(decoding: head.prefix(while: { $0 != 13 }), as: UTF8.self)
        let parts = line.split(separator: " ")
        var path = parts.count >= 2 ? String(parts[1]) : "/"
        if let query = path.firstIndex(of: "?") {
            path = String(path[..<query])
        }
        let file = files.resolve(path)
        let size = file.flatMap { try? $0.resourceValues(forKeys: [.fileSizeKey]).fileSize } ?? 0
        let handle = file.flatMap { try? FileHandle(forReadingFrom: $0) }
        var headers = WebFiles.isolation
        headers["Content-Type"] = file.map(WebFiles.type(of:)) ?? "text/plain"
        headers["Connection"] = "close"
        headers["Accept-Ranges"] = "bytes"
        var status = handle == nil ? "404 Not Found" : "200 OK"
        var remaining = handle == nil ? 0 : size
        // A range asked for, as "bytes=first-last" or "bytes=first-": only that part is sent.
        if let handle, let range = Self.range(in: head, size: size) {
            try? handle.seek(toOffset: UInt64(range.lowerBound))
            remaining = range.count
            status = "206 Partial Content"
            headers["Content-Range"] = "bytes \(range.lowerBound)-\(range.upperBound - 1)/\(size)"
        }
        let chunked = handle != nil
        if chunked {
            headers["Transfer-Encoding"] = "chunked"
            headers["Geometry-Size"] = String(remaining)
        } else {
            headers["Content-Length"] = "0"
        }
        let text = (["HTTP/1.1 \(status)"] + headers.map { "\($0.key): \($0.value)" })
            .joined(separator: "\r\n") + "\r\n\r\n"
        connection.send(content: Data(text.utf8), completion: .contentProcessed { [self, remaining] _ in
            pump(connection, handle, remaining: remaining, chunked: chunked)
        })
    }

    // The byte range a request's Range header asks for, within `size`, or nil for the whole file.
    private static func range(in head: Data, size: Int) -> Range<Int>? {
        let lines = String(decoding: head, as: UTF8.self).split(separator: "\r\n")
        guard let line = lines.first(where: { $0.lowercased().hasPrefix("range:") }),
            let equals = line.firstIndex(of: "=")
        else {
            return nil
        }
        let bounds = line[line.index(after: equals)...].split(separator: "-", omittingEmptySubsequences: false)
        guard bounds.count == 2, let first = Int(bounds[0].trimmingCharacters(in: .whitespaces)),
            first < size
        else {
            return nil
        }
        let last = Int(bounds[1].trimmingCharacters(in: .whitespaces)).map { min($0, size - 1) }
            ?? size - 1
        return last >= first ? first..<(last + 1) : nil
    }

    // Sends the file a chunk at a time, each after the last is handed to the network stack, until
    // `remaining` bytes have gone. A read that fails ends the body short, without its last chunk,
    // which the page sees as a failed read rather than a shorter file.
    private func pump(
        _ connection: NWConnection,
        _ handle: FileHandle?,
        remaining: Int,
        chunked: Bool
    ) {
        guard let handle, remaining > 0,
            let chunk = try? handle.read(upToCount: min(2 << 20, remaining)), !chunk.isEmpty
        else {
            try? handle?.close()
            let last = chunked && remaining == 0 ? Data("0\r\n\r\n".utf8) : nil
            connection.send(content: last, contentContext: .finalMessage, isComplete: true,
                            completion: .contentProcessed { _ in connection.cancel() })
            return
        }
        var framed = Data("\(String(chunk.count, radix: 16))\r\n".utf8)
        framed.append(chunk)
        framed.append(contentsOf: [13, 10])
        connection.send(content: framed, completion: .contentProcessed { [self] error in
            if error != nil {
                try? handle.close()
                connection.cancel()
            } else {
                pump(connection, handle, remaining: remaining - chunk.count, chunked: chunked)
            }
        })
    }
}
