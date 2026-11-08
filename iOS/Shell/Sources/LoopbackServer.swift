import Foundation
import Network

/// A minimal HTTP server on the phone's own loopback address, serving the shell's web files.
///
/// WebKit grants cross-origin isolation, which WebAssembly threads need, only to pages whose
/// responses carry the isolation headers over HTTP: the geo:// scheme handler's are not honoured.
/// 127.0.0.1 is a secure context, never leaves the device, and needs no network permission.
/// Each connection answers one GET and closes; files stream in 2 MB chunks. Everything runs on one
/// serial queue, which is what makes it safe to share.
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
        let size = file.flatMap { try? $0.resourceValues(forKeys: [.fileSizeKey]).fileSize }
        let handle = file.flatMap { try? FileHandle(forReadingFrom: $0) }
        var headers = WebFiles.isolation
        headers["Content-Type"] = file.map(WebFiles.type(of:)) ?? "text/plain"
        headers["Content-Length"] = String(handle == nil ? 0 : size ?? 0)
        headers["Connection"] = "close"
        let status = handle == nil ? "404 Not Found" : "200 OK"
        let text = (["HTTP/1.1 \(status)"] + headers.map { "\($0.key): \($0.value)" })
            .joined(separator: "\r\n") + "\r\n\r\n"
        connection.send(content: Data(text.utf8), completion: .contentProcessed { [self] _ in
            pump(connection, handle)
        })
    }

    // Sends the file a chunk at a time, each after the last is handed to the network stack.
    private func pump(_ connection: NWConnection, _ handle: FileHandle?) {
        guard let handle, let chunk = try? handle.read(upToCount: 2 << 20), !chunk.isEmpty else {
            try? handle?.close()
            connection.send(content: nil, contentContext: .finalMessage, isComplete: true,
                            completion: .contentProcessed { _ in connection.cancel() })
            return
        }
        connection.send(content: chunk, completion: .contentProcessed { [self] error in
            if error != nil {
                try? handle.close()
                connection.cancel()
            } else {
                pump(connection, handle)
            }
        })
    }
}
