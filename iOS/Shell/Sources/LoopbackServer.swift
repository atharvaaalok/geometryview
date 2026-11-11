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
/// GeometryView-Size instead, for the viewer to bound what a file's header may ask for.
final class LoopbackServer: @unchecked Sendable {
    private let files: WebFiles
    private var listener: NWListener
    private var onUsualPort = true
    private let queue = DispatchQueue(label: "geo.loopback")

    /// The port the viewer is served on. The same one each launch keeps the page's origin, and so
    /// what it keeps in the browser's storage, its kept imports and Recent among it; on any port
    /// it would start each launch with none.
    static let usualPort: UInt16 = 47701

    init(files: WebFiles) throws {
        self.files = files
        listener = try Self.listener(on: NWEndpoint.Port(rawValue: Self.usualPort) ?? .any)
    }

    private static func listener(on port: NWEndpoint.Port) throws -> NWListener {
        // Small writes go out at once, rather than waiting on the reader's delayed acknowledgement.
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true
        let parameters = NWParameters(tls: nil, tcp: tcp)
        parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: port)
        parameters.allowLocalEndpointReuse = true
        return try NWListener(using: parameters)
    }

    /// Starts listening and reports the port, once, when it is known: the usual one, or if that is
    /// taken, any.
    func start(ready: @escaping @Sendable (UInt16) -> Void) {
        let listener = listener
        listener.stateUpdateHandler = { [self, listener] state in
            switch state {
            case .ready:
                if let port = listener.port {
                    ready(port.rawValue)
                }
            case .failed:
                listener.cancel()
                if onUsualPort, let fallback = try? Self.listener(on: .any) {
                    onUsualPort = false
                    self.listener = fallback
                    start(ready: ready)
                }
            default:
                break
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
        var query: String?
        if let mark = path.firstIndex(of: "?") {
            query = String(path[path.index(after: mark)...])
            path = String(path[..<mark])
        }
        if let scan = files.scan(path, query: query) {
            var headers = WebFiles.isolation
            headers["Content-Type"] = "application/x-ndjson"
            headers["Connection"] = "close"
            headers["Content-Length"] = String(scan.count)
            let text = (["HTTP/1.1 200 OK"] + headers.map { "\($0.key): \($0.value)" })
                .joined(separator: "\r\n") + "\r\n\r\n"
            connection.send(content: Data(text.utf8) + scan, completion: .contentProcessed { _ in
                connection.cancel()
            })
            return
        }
        if let listing = files.listing(path) {
            var headers = WebFiles.isolation
            headers["Content-Type"] = "application/json"
            headers["Connection"] = "close"
            headers["Content-Length"] = String(listing.count)
            let text = (["HTTP/1.1 200 OK"] + headers.map { "\($0.key): \($0.value)" })
                .joined(separator: "\r\n") + "\r\n\r\n"
            connection.send(content: Data(text.utf8) + listing, completion: .contentProcessed { _ in
                connection.cancel()
            })
            return
        }
        let file = files.resolve(path)
        let size = file.flatMap { try? $0.resourceValues(forKeys: [.fileSizeKey]).fileSize } ?? 0
        let handle = file.flatMap { try? FileHandle(forReadingFrom: $0) }
        var headers = WebFiles.isolation
        headers["Content-Type"] = file.map(WebFiles.type(of:)) ?? "text/plain"
        headers["Connection"] = "close"
        headers["Accept-Ranges"] = "bytes"
        if path.hasPrefix("/open/"), let file, let modified = WebFiles.modified(file) {
            headers["GeometryView-Modified"] = modified
        }
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
            headers["GeometryView-Size"] = String(remaining)
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
