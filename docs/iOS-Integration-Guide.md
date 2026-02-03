# Gaze Stream iOS Integration Guide

This document describes how to integrate Gaze Stream reception into an iOS application for low-latency video streaming from OBS.

## Overview

Gaze Stream is a UDP-based video streaming protocol optimized for low latency. It uses:
- **mDNS** for service discovery
- **RTP** for video packet transport
- **HEVC/H.264** for video encoding
- **Reed-Solomon FEC** for packet loss recovery (optional)

## Architecture

```
┌─────────────────┐         ┌─────────────────┐
│   OBS Studio    │         │    iOS App      │
│  (Gaze Filter)  │         │   (Receiver)    │
├─────────────────┤         ├─────────────────┤
│ mDNS Advertiser │◄───────►│ mDNS Browser    │
│ Control Port    │◄────────│ Control Client  │
│ RTP Sender      │────────►│ RTP Receiver    │
└─────────────────┘         └─────────────────┘
```

## Protocol Constants

```swift
// Network
let GAZE_BASE_PORT: UInt16 = 5960          // Output 1: 5960, Output 2: 5962, etc.
let GAZE_MTU = 1500
let GAZE_MAX_PAYLOAD_SIZE = 1400

// mDNS
let GAZE_MDNS_SERVICE_TYPE = "_gazestream._udp"

// Control Messages
let GAZE_CTRL_MAGIC: [UInt8] = [0x47, 0x5A]  // "GZ"
let GAZE_CTRL_SUBSCRIBE: UInt8 = 0x01
let GAZE_CTRL_HEARTBEAT: UInt8 = 0x02
let GAZE_CTRL_UNSUBSCRIBE: UInt8 = 0x03
let GAZE_CTRL_PROBE: UInt8 = 0x04
let GAZE_CTRL_PROBE_RESPONSE: UInt8 = 0x05

// Probe Flags
let GAZE_PROBE_FLAG_REQUEST_FRAME: UInt8 = 0x01

// Probe Response Status
let GAZE_PROBE_STATUS_ACTIVE: UInt8 = 0x01
let GAZE_PROBE_STATUS_HAS_RECEIVERS: UInt8 = 0x02
let GAZE_PROBE_STATUS_FRAME_INCLUDED: UInt8 = 0x04

// Timing
let GAZE_HEARTBEAT_INTERVAL: TimeInterval = 1.0  // Send every 1 second
let GAZE_RECEIVER_TIMEOUT: TimeInterval = 5.0    // Sender drops after 5s no heartbeat

// RTP
let GAZE_RTP_PAYLOAD_TYPE_HEVC: UInt8 = 96
let GAZE_RTP_PAYLOAD_TYPE_H264: UInt8 = 97
```

## Step 1: Service Discovery (mDNS)

Use `NetServiceBrowser` to discover Gaze Stream services:

```swift
import Foundation

class GazeDiscovery: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    private let browser = NetServiceBrowser()
    private var discoveredServices: [NetService] = []

    var onServiceFound: ((GazeStreamInfo) -> Void)?
    var onServiceLost: ((String) -> Void)?

    func startDiscovery() {
        browser.delegate = self
        browser.searchForServices(ofType: "_gazestream._udp.", inDomain: "local.")
    }

    func stopDiscovery() {
        browser.stop()
    }

    // MARK: - NetServiceBrowserDelegate

    func netServiceBrowser(_ browser: NetServiceBrowser,
                          didFind service: NetService,
                          moreComing: Bool) {
        discoveredServices.append(service)
        service.delegate = self
        service.resolve(withTimeout: 5.0)
    }

    func netServiceBrowser(_ browser: NetServiceBrowser,
                          didRemove service: NetService,
                          moreComing: Bool) {
        discoveredServices.removeAll { $0 == service }
        onServiceLost?(service.name)
    }

    // MARK: - NetServiceDelegate

    func netServiceDidResolveAddress(_ sender: NetService) {
        guard let addresses = sender.addresses,
              let txtData = sender.txtRecordData() else { return }

        // Parse TXT record
        let txtDict = NetService.dictionary(fromTXTRecord: txtData)

        let info = GazeStreamInfo(
            name: sender.name,
            host: resolveHost(from: addresses),
            port: UInt16(sender.port),
            codec: String(data: txtDict["codec"] ?? Data(), encoding: .utf8) ?? "hevc",
            width: UInt32(String(data: txtDict["width"] ?? Data(), encoding: .utf8) ?? "0") ?? 0,
            height: UInt32(String(data: txtDict["height"] ?? Data(), encoding: .utf8) ?? "0") ?? 0,
            fps: UInt32(String(data: txtDict["fps"] ?? Data(), encoding: .utf8) ?? "0") ?? 0
        )

        onServiceFound?(info)
    }

    private func resolveHost(from addresses: [Data]) -> String {
        for addr in addresses {
            var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            addr.withUnsafeBytes { ptr in
                let sockaddr = ptr.baseAddress!.assumingMemoryBound(to: sockaddr.self)
                getnameinfo(sockaddr, socklen_t(addr.count),
                           &hostname, socklen_t(hostname.count),
                           nil, 0, NI_NUMERICHOST)
            }
            let host = String(cString: hostname)
            // Prefer IPv4
            if !host.contains(":") {
                return host
            }
        }
        return ""
    }
}

struct GazeStreamInfo {
    let name: String
    let host: String
    let port: UInt16
    let codec: String      // "hevc" or "h264"
    let width: UInt32
    let height: UInt32
    let fps: UInt32

    var controlPort: UInt16 { port + 1 }
    var isHEVC: Bool { codec.lowercased() == "hevc" }
}
```

## Step 2: Probe Stream (Optional Preview)

Before subscribing, you can probe a stream to check status and get a preview frame:

```swift
class GazeProbe {

    struct ProbeResponse {
        let isActive: Bool
        let hasReceivers: Bool
        let width: UInt16
        let height: UInt16
        let fpsNum: UInt16
        let fpsDen: UInt16
        let frameCount: UInt32
        let frameData: Data?      // HEVC/H264 encoded keyframe
    }

    static func probe(host: String, port: UInt16,
                     requestFrame: Bool = true,
                     timeout: TimeInterval = 2.0) async -> ProbeResponse? {

        let controlPort = port + 1

        // Create UDP socket
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return nil }
        defer { close(fd) }

        // Set timeout
        var tv = timeval(tv_sec: Int(timeout), tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        // Bind to any port
        var bindAddr = sockaddr_in()
        bindAddr.sin_family = sa_family_t(AF_INET)
        bindAddr.sin_port = 0
        bindAddr.sin_addr.s_addr = INADDR_ANY

        let bindResult = withUnsafePointer(to: &bindAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindResult == 0 else { return nil }

        // Build probe request
        let requestId = UInt32.random(in: 0...UInt32.max)
        var request = Data()
        request.append(contentsOf: GAZE_CTRL_MAGIC)
        request.append(GAZE_CTRL_PROBE)
        request.append(requestFrame ? GAZE_PROBE_FLAG_REQUEST_FRAME : 0)
        request.append(contentsOf: withUnsafeBytes(of: requestId.bigEndian) { Array($0) })

        // Send to control port
        var destAddr = sockaddr_in()
        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port = controlPort.bigEndian
        inet_pton(AF_INET, host, &destAddr.sin_addr)

        let sent = request.withUnsafeBytes { ptr in
            withUnsafePointer(to: &destAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { addr in
                    sendto(fd, ptr.baseAddress, request.count, 0, addr,
                          socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        guard sent > 0 else { return nil }

        // Receive response (may be large with keyframe)
        var buffer = [UInt8](repeating: 0, count: 2 * 1024 * 1024)  // 2MB max
        let received = recv(fd, &buffer, buffer.count, 0)
        guard received >= 24 else { return nil }

        // Parse response
        let data = Data(bytes: buffer, count: received)
        return parseProbeResponse(data, expectedRequestId: requestId)
    }

    private static func parseProbeResponse(_ data: Data,
                                           expectedRequestId: UInt32) -> ProbeResponse? {
        guard data.count >= 24,
              data[0] == GAZE_CTRL_MAGIC[0],
              data[1] == GAZE_CTRL_MAGIC[1],
              data[2] == GAZE_CTRL_PROBE_RESPONSE else { return nil }

        let status = data[3]
        let requestId = data.subdata(in: 4..<8).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }

        guard requestId == expectedRequestId else { return nil }

        let width = data.subdata(in: 8..<10).withUnsafeBytes {
            UInt16(bigEndian: $0.load(as: UInt16.self))
        }
        let height = data.subdata(in: 10..<12).withUnsafeBytes {
            UInt16(bigEndian: $0.load(as: UInt16.self))
        }
        let fpsNum = data.subdata(in: 12..<14).withUnsafeBytes {
            UInt16(bigEndian: $0.load(as: UInt16.self))
        }
        let fpsDen = data.subdata(in: 14..<16).withUnsafeBytes {
            UInt16(bigEndian: $0.load(as: UInt16.self))
        }
        let frameCount = data.subdata(in: 16..<20).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }
        let frameDataSize = data.subdata(in: 20..<24).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }

        var frameData: Data? = nil
        if (status & GAZE_PROBE_STATUS_FRAME_INCLUDED) != 0 && frameDataSize > 0 {
            let endIndex = min(24 + Int(frameDataSize), data.count)
            frameData = data.subdata(in: 24..<endIndex)
        }

        return ProbeResponse(
            isActive: (status & GAZE_PROBE_STATUS_ACTIVE) != 0,
            hasReceivers: (status & GAZE_PROBE_STATUS_HAS_RECEIVERS) != 0,
            width: width,
            height: height,
            fpsNum: fpsNum,
            fpsDen: fpsDen,
            frameCount: frameCount,
            frameData: frameData
        )
    }
}
```

## Step 3: Subscribe and Receive Video

### 3.1 Control Message Handling

```swift
class GazeReceiver {
    private var socket: Int32 = -1
    private var receiveQueue: DispatchQueue?
    private var heartbeatTimer: Timer?
    private var isRunning = false

    let host: String
    let rtpPort: UInt16
    var controlPort: UInt16 { rtpPort + 1 }

    var onFrameReceived: ((GazeFrame) -> Void)?

    init(host: String, port: UInt16) {
        self.host = host
        self.rtpPort = port
    }

    func start() {
        guard !isRunning else { return }

        // Create UDP socket
        socket = Darwin.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socket >= 0 else { return }

        // Set large receive buffer (4MB for keyframes)
        var bufferSize: Int32 = 4 * 1024 * 1024
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &bufferSize,
                  socklen_t(MemoryLayout<Int32>.size))

        // Bind to RTP port to receive video
        var bindAddr = sockaddr_in()
        bindAddr.sin_family = sa_family_t(AF_INET)
        bindAddr.sin_port = rtpPort.bigEndian
        bindAddr.sin_addr.s_addr = INADDR_ANY

        let bindResult = withUnsafePointer(to: &bindAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(socket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        guard bindResult == 0 else {
            close(socket)
            return
        }

        isRunning = true

        // Send subscribe message
        sendControlMessage(GAZE_CTRL_SUBSCRIBE)

        // Start heartbeat timer
        heartbeatTimer = Timer.scheduledTimer(withTimeInterval: GAZE_HEARTBEAT_INTERVAL,
                                              repeats: true) { [weak self] _ in
            self?.sendControlMessage(GAZE_CTRL_HEARTBEAT)
        }

        // Start receive loop
        receiveQueue = DispatchQueue(label: "gaze.receive", qos: .userInteractive)
        receiveQueue?.async { [weak self] in
            self?.receiveLoop()
        }
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false

        heartbeatTimer?.invalidate()
        heartbeatTimer = nil

        // Send unsubscribe
        sendControlMessage(GAZE_CTRL_UNSUBSCRIBE)

        close(socket)
        socket = -1
    }

    private func sendControlMessage(_ type: UInt8) {
        var message = Data()
        message.append(contentsOf: GAZE_CTRL_MAGIC)
        message.append(type)
        message.append(0x00)  // Padding

        var destAddr = sockaddr_in()
        destAddr.sin_family = sa_family_t(AF_INET)
        destAddr.sin_port = controlPort.bigEndian
        inet_pton(AF_INET, host, &destAddr.sin_addr)

        message.withUnsafeBytes { ptr in
            withUnsafePointer(to: &destAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { addr in
                    sendto(socket, ptr.baseAddress, message.count, 0, addr,
                          socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    private func receiveLoop() {
        var buffer = [UInt8](repeating: 0, count: Int(GAZE_MTU))

        while isRunning {
            let received = recv(socket, &buffer, buffer.count, 0)
            if received > 0 {
                let data = Data(bytes: buffer, count: received)
                processPacket(data)
            }
        }
    }

    // ... packet processing (see next section)
}
```

### 3.2 Packet Parsing and Frame Assembly

```swift
// Packet structures
struct GazeRTPHeader {
    let version: UInt8
    let marker: Bool           // Last packet of frame
    let payloadType: UInt8     // 96=HEVC, 97=H264
    let sequence: UInt16
    let timestamp: UInt32
    let ssrc: UInt32
}

struct GazeFrameMeta {
    let frameIndex: UInt32
    let fecType: UInt8
    let fecBlockIndex: UInt8
    let fecDataShards: UInt8
    let fecParityShards: UInt8
}

struct GazeFrameHeader {
    let headerType: UInt8      // 0x01=video, 0x02=FEC
    let frameType: UInt8       // 0=IDR(keyframe), 1=P, 2=B
    let flags: UInt16
    let captureTimestampMs: UInt32
}

struct GazeFrame {
    let index: UInt32
    let isKeyframe: Bool
    let codec: GazeCodec
    let captureTimestampMs: UInt32
    let data: Data
}

enum GazeCodec {
    case hevc
    case h264
}

// Frame assembler
class GazeFrameAssembler {
    private var assemblers: [UInt32: FrameBuffer] = [:]
    private var lastCompletedFrame: Int64 = -1

    struct FrameBuffer {
        let frameIndex: UInt32
        let isKeyframe: Bool
        let codec: GazeCodec
        let captureTimestampMs: UInt32
        var packets: [UInt16: Data] = [:]
        var fecSeqs: Set<UInt16> = []
        var receivedMarker = false
        var totalDataPackets = 0
    }

    func processPacket(_ data: Data) -> GazeFrame? {
        guard data.count >= 28 else { return nil }

        // Parse RTP header (12 bytes)
        let flags = data[0]
        let version = (flags >> 6) & 0x03
        guard version == 2 else { return nil }  // RTP version 2

        let pt = data[1]
        let marker = (pt & 0x80) != 0
        let payloadType = pt & 0x7F

        let sequence = data.subdata(in: 2..<4).withUnsafeBytes {
            UInt16(bigEndian: $0.load(as: UInt16.self))
        }
        let timestamp = data.subdata(in: 4..<8).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }

        // Parse frame meta (8 bytes at offset 12)
        let frameIndex = data.subdata(in: 12..<16).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }
        let fecType = data[16]
        let fecBlockIndex = data[17]
        let fecDataShards = data[18]
        let fecParityShards = data[19]

        // Parse frame header (8 bytes at offset 20)
        let headerType = data[20]
        let frameType = data[21]
        let captureTs = data.subdata(in: 24..<28).withUnsafeBytes {
            UInt32(bigEndian: $0.load(as: UInt32.self))
        }

        // Skip FEC packets for now (headerType == 0x02)
        if headerType == 0x02 {
            assemblers[frameIndex]?.fecSeqs.insert(sequence)
            return nil
        }

        // Skip old frames
        if Int64(frameIndex) <= lastCompletedFrame {
            return nil
        }

        let codec: GazeCodec = payloadType == GAZE_RTP_PAYLOAD_TYPE_HEVC ? .hevc : .h264
        let isKeyframe = frameType == 0

        // Get or create assembler
        if assemblers[frameIndex] == nil {
            assemblers[frameIndex] = FrameBuffer(
                frameIndex: frameIndex,
                isKeyframe: isKeyframe,
                codec: codec,
                captureTimestampMs: captureTs
            )
        }

        // Add packet payload (offset 28+)
        let payload = data.subdata(in: 28..<data.count)
        assemblers[frameIndex]?.packets[sequence] = payload

        if marker {
            assemblers[frameIndex]?.receivedMarker = true
            // Calculate expected packets from FEC metadata
            assemblers[frameIndex]?.totalDataPackets = Int(fecDataShards)
        }

        // Check if complete
        if let buffer = assemblers[frameIndex], isFrameComplete(buffer) {
            let frame = assembleFrame(buffer)
            lastCompletedFrame = Int64(frameIndex)
            assemblers.removeValue(forKey: frameIndex)
            cleanupOldAssemblers(currentFrame: frameIndex)
            return frame
        }

        return nil
    }

    private func isFrameComplete(_ buffer: FrameBuffer) -> Bool {
        guard buffer.receivedMarker else { return false }
        guard !buffer.packets.isEmpty else { return false }

        // Check we have all expected data packets
        if buffer.totalDataPackets > 0 {
            if buffer.packets.count < buffer.totalDataPackets {
                return false
            }
        }

        // Check for gaps (excluding FEC sequences)
        let seqs = buffer.packets.keys.sorted()
        guard let minSeq = seqs.first, let maxSeq = seqs.last else { return false }

        for seq in minSeq...maxSeq {
            if !buffer.packets.keys.contains(seq) && !buffer.fecSeqs.contains(seq) {
                return false
            }
        }

        return true
    }

    private func assembleFrame(_ buffer: FrameBuffer) -> GazeFrame {
        let sortedPayloads = buffer.packets.keys.sorted().compactMap { buffer.packets[$0] }
        let frameData = sortedPayloads.reduce(Data()) { $0 + $1 }

        return GazeFrame(
            index: buffer.frameIndex,
            isKeyframe: buffer.isKeyframe,
            codec: buffer.codec,
            captureTimestampMs: buffer.captureTimestampMs,
            data: frameData
        )
    }

    private func cleanupOldAssemblers(currentFrame: UInt32) {
        let threshold = currentFrame > 30 ? currentFrame - 30 : 0
        assemblers = assemblers.filter { $0.key >= threshold }
    }
}
```

## Step 4: Hardware Video Decoding

Use VideoToolbox for hardware-accelerated decoding:

```swift
import VideoToolbox
import CoreMedia

class GazeDecoder {
    private var decompressionSession: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?
    private var codec: GazeCodec = .hevc

    var onFrameDecoded: ((CVPixelBuffer, CMTime) -> Void)?

    func decode(frame: GazeFrame) {
        // Reinitialize if codec changed
        if codec != frame.codec {
            reset()
            codec = frame.codec
        }

        // Create format description from keyframe if needed
        if formatDescription == nil && frame.isKeyframe {
            formatDescription = createFormatDescription(from: frame.data, codec: frame.codec)
            if formatDescription != nil {
                createDecompressionSession()
            }
        }

        guard let session = decompressionSession,
              let formatDesc = formatDescription else { return }

        // Create block buffer from frame data
        var blockBuffer: CMBlockBuffer?
        frame.data.withUnsafeBytes { ptr in
            CMBlockBufferCreateWithMemoryBlock(
                allocator: kCFAllocatorDefault,
                memoryBlock: UnsafeMutableRawPointer(mutating: ptr.baseAddress),
                blockLength: frame.data.count,
                blockAllocator: kCFAllocatorNull,
                customBlockSource: nil,
                offsetToData: 0,
                dataLength: frame.data.count,
                flags: 0,
                blockBufferOut: &blockBuffer
            )
        }

        guard let buffer = blockBuffer else { return }

        // Create sample buffer
        var sampleBuffer: CMSampleBuffer?
        var sampleSize = frame.data.count

        CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: buffer,
            formatDescription: formatDesc,
            sampleCount: 1,
            sampleTimingEntryCount: 0,
            sampleTimingArray: nil,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )

        guard let sample = sampleBuffer else { return }

        // Decode
        var flagsOut: VTDecodeInfoFlags = []
        let timestamp = CMTime(value: Int64(frame.captureTimestampMs), timescale: 1000)

        VTDecompressionSessionDecodeFrame(
            session,
            sampleBuffer: sample,
            flags: [._EnableAsynchronousDecompression],
            infoFlagsOut: &flagsOut
        ) { [weak self] status, _, imageBuffer, presentationTime, _ in
            if status == noErr, let pixelBuffer = imageBuffer {
                self?.onFrameDecoded?(pixelBuffer, timestamp)
            }
        }
    }

    private func createFormatDescription(from data: Data, codec: GazeCodec) -> CMVideoFormatDescription? {
        // Parse NAL units to extract parameter sets
        let nalUnits = parseAnnexBNALUnits(data)

        if codec == .hevc {
            return createHEVCFormatDescription(nalUnits: nalUnits)
        } else {
            return createH264FormatDescription(nalUnits: nalUnits)
        }
    }

    private func parseAnnexBNALUnits(_ data: Data) -> [[UInt8]] {
        var units: [[UInt8]] = []
        var i = 0
        let bytes = [UInt8](data)

        while i < bytes.count - 4 {
            // Find start code (00 00 00 01 or 00 00 01)
            if bytes[i] == 0 && bytes[i+1] == 0 {
                var startCodeLen = 0
                if bytes[i+2] == 0 && bytes[i+3] == 1 {
                    startCodeLen = 4
                } else if bytes[i+2] == 1 {
                    startCodeLen = 3
                }

                if startCodeLen > 0 {
                    // Find next start code
                    var end = i + startCodeLen
                    while end < bytes.count - 3 {
                        if bytes[end] == 0 && bytes[end+1] == 0 &&
                           (bytes[end+2] == 1 || (bytes[end+2] == 0 && bytes[end+3] == 1)) {
                            break
                        }
                        end += 1
                    }
                    if end >= bytes.count - 3 { end = bytes.count }

                    let unit = Array(bytes[(i + startCodeLen)..<end])
                    if !unit.isEmpty {
                        units.append(unit)
                    }
                    i = end
                    continue
                }
            }
            i += 1
        }

        return units
    }

    private func createHEVCFormatDescription(nalUnits: [[UInt8]]) -> CMVideoFormatDescription? {
        // HEVC NAL types: VPS=32, SPS=33, PPS=34
        var vps: [UInt8]?
        var sps: [UInt8]?
        var pps: [UInt8]?

        for unit in nalUnits {
            guard !unit.isEmpty else { continue }
            let nalType = (unit[0] >> 1) & 0x3F
            switch nalType {
            case 32: vps = unit
            case 33: sps = unit
            case 34: pps = unit
            default: break
            }
        }

        guard let vpsData = vps, let spsData = sps, let ppsData = pps else { return nil }

        let parameterSets: [[UInt8]] = [vpsData, spsData, ppsData]
        let sizes = parameterSets.map { $0.count }

        var formatDesc: CMFormatDescription?

        parameterSets.withUnsafeBufferPointer { setsPtr in
            sizes.withUnsafeBufferPointer { sizesPtr in
                let pointers = setsPtr.map { UnsafePointer($0) }
                pointers.withUnsafeBufferPointer { pointersPtr in
                    CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 3,
                        parameterSetPointers: pointersPtr.baseAddress!,
                        parameterSetSizes: sizesPtr.baseAddress!,
                        nalUnitHeaderLength: 4,
                        extensions: nil,
                        formatDescriptionOut: &formatDesc
                    )
                }
            }
        }

        return formatDesc
    }

    private func createH264FormatDescription(nalUnits: [[UInt8]]) -> CMVideoFormatDescription? {
        // H.264 NAL types: SPS=7, PPS=8
        var sps: [UInt8]?
        var pps: [UInt8]?

        for unit in nalUnits {
            guard !unit.isEmpty else { continue }
            let nalType = unit[0] & 0x1F
            switch nalType {
            case 7: sps = unit
            case 8: pps = unit
            default: break
            }
        }

        guard let spsData = sps, let ppsData = pps else { return nil }

        let parameterSets: [[UInt8]] = [spsData, ppsData]
        let sizes = parameterSets.map { $0.count }

        var formatDesc: CMFormatDescription?

        parameterSets.withUnsafeBufferPointer { setsPtr in
            sizes.withUnsafeBufferPointer { sizesPtr in
                let pointers = setsPtr.map { UnsafePointer($0) }
                pointers.withUnsafeBufferPointer { pointersPtr in
                    CMVideoFormatDescriptionCreateFromH264ParameterSets(
                        allocator: kCFAllocatorDefault,
                        parameterSetCount: 2,
                        parameterSetPointers: pointersPtr.baseAddress!,
                        parameterSetSizes: sizesPtr.baseAddress!,
                        nalUnitHeaderLength: 4,
                        formatDescriptionOut: &formatDesc
                    )
                }
            }
        }

        return formatDesc
    }

    private func createDecompressionSession() {
        guard let formatDesc = formatDescription else { return }

        let decoderSpecification: [String: Any] = [
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder as String: true
        ]

        let destinationAttributes: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferMetalCompatibilityKey as String: true
        ]

        var session: VTDecompressionSession?
        VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: formatDesc,
            decoderSpecification: decoderSpecification as CFDictionary,
            imageBufferAttributes: destinationAttributes as CFDictionary,
            outputCallback: nil,
            decompressionSessionOut: &session
        )

        decompressionSession = session
    }

    func reset() {
        if let session = decompressionSession {
            VTDecompressionSessionInvalidate(session)
        }
        decompressionSession = nil
        formatDescription = nil
    }
}
```

## Step 5: Display with Metal

```swift
import MetalKit

class GazeMetalView: MTKView {
    private var commandQueue: MTLCommandQueue?
    private var textureCache: CVMetalTextureCache?
    private var pipelineState: MTLRenderPipelineState?
    private var currentTexture: MTLTexture?

    func configure() {
        device = MTLCreateSystemDefaultDevice()
        guard let device = device else { return }

        commandQueue = device.makeCommandQueue()

        CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &textureCache)

        // Create render pipeline (simplified - you'd load shaders)
        // ...

        framebufferOnly = false
        isPaused = false
        enableSetNeedsDisplay = false
    }

    func display(pixelBuffer: CVPixelBuffer) {
        guard let textureCache = textureCache else { return }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)

        var cvTexture: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault,
            textureCache,
            pixelBuffer,
            nil,
            .bgra8Unorm,
            width,
            height,
            0,
            &cvTexture
        )

        guard let texture = cvTexture,
              let metalTexture = CVMetalTextureGetTexture(texture) else { return }

        currentTexture = metalTexture
        draw()
    }

    override func draw(_ rect: CGRect) {
        guard let texture = currentTexture,
              let drawable = currentDrawable,
              let commandBuffer = commandQueue?.makeCommandBuffer() else { return }

        // Blit texture to drawable (simplified)
        let blitEncoder = commandBuffer.makeBlitCommandEncoder()
        blitEncoder?.copy(from: texture, to: drawable.texture)
        blitEncoder?.endEncoding()

        commandBuffer.present(drawable)
        commandBuffer.commit()
    }
}
```

## Complete Usage Example

```swift
class GazeStreamViewController: UIViewController {
    private var discovery: GazeDiscovery?
    private var receiver: GazeReceiver?
    private var assembler = GazeFrameAssembler()
    private var decoder = GazeDecoder()
    private var metalView: GazeMetalView!

    override func viewDidLoad() {
        super.viewDidLoad()

        // Setup Metal view
        metalView = GazeMetalView(frame: view.bounds)
        metalView.configure()
        view.addSubview(metalView)

        // Setup decoder callback
        decoder.onFrameDecoded = { [weak self] pixelBuffer, _ in
            DispatchQueue.main.async {
                self?.metalView.display(pixelBuffer: pixelBuffer)
            }
        }

        // Start discovery
        discovery = GazeDiscovery()
        discovery?.onServiceFound = { [weak self] info in
            print("Found stream: \(info.name) at \(info.host):\(info.port)")
            // Auto-connect to first stream (or show UI to select)
            self?.connect(to: info)
        }
        discovery?.startDiscovery()
    }

    func connect(to stream: GazeStreamInfo) {
        // Optional: Probe first to check status
        Task {
            if let probe = await GazeProbe.probe(host: stream.host, port: stream.port) {
                print("Stream active: \(probe.isActive), resolution: \(probe.width)x\(probe.height)")
            }
        }

        // Start receiving
        receiver = GazeReceiver(host: stream.host, port: stream.port)
        receiver?.onPacketReceived = { [weak self] data in
            if let frame = self?.assembler.processPacket(data) {
                self?.decoder.decode(frame: frame)
            }
        }
        receiver?.start()
    }

    func disconnect() {
        receiver?.stop()
        receiver = nil
        decoder.reset()
    }

    deinit {
        disconnect()
        discovery?.stopDiscovery()
    }
}
```

## Protocol Reference

### Control Port Messages (RTP Port + 1)

| Message | Bytes | Direction | Description |
|---------|-------|-----------|-------------|
| SUBSCRIBE | `GZ 01 00` | Receiver → Sender | Start receiving stream |
| HEARTBEAT | `GZ 02 00` | Receiver → Sender | Keep subscription alive (every 1s) |
| UNSUBSCRIBE | `GZ 03 00` | Receiver → Sender | Stop receiving stream |
| PROBE | `GZ 04 FF IIII` | Receiver → Sender | Request status (FF=flags, IIII=request ID) |
| PROBE_RESPONSE | `GZ 05 SS IIII WWHHFFFF CCCC DDDD [data]` | Sender → Receiver | Status response |

### RTP Video Packet (28-byte header + payload)

```
Offset  Size  Field
0       1     RTP flags (version=2, padding, extension, CSRC count)
1       1     Marker bit (MSB) + Payload type (96=HEVC, 97=H264)
2       2     Sequence number (big-endian)
4       4     RTP timestamp (big-endian)
8       4     SSRC (big-endian)
12      4     Frame index (big-endian)
16      1     FEC type (0=none, 1=Reed-Solomon)
17      1     FEC block index
18      1     FEC data shards count
19      1     FEC parity shards count
20      1     Packet type (0x01=video, 0x02=FEC)
21      1     Frame type (0=IDR/keyframe, 1=P, 2=B)
22      2     Flags (reserved)
24      4     Capture timestamp ms (big-endian)
28+     N     Video payload (HEVC/H264 NAL units, Annex B format)
```

## Tips for iOS Implementation

1. **Background Modes**: Add "Audio, AirPlay, and Picture in Picture" background mode if you need background streaming.

2. **Network Permissions**: Add `NSLocalNetworkUsageDescription` to Info.plist for mDNS discovery.

3. **Low Latency**: Use `CADisplayLink` for frame pacing instead of timers.

4. **Memory**: Set `CVPixelBufferPoolCreate` for decoder output to reuse buffers.

5. **Thread Safety**: Decode on a dedicated queue, display on main thread.

6. **Error Recovery**: Request keyframe by reconnecting if decoder fails (stop/start receiver).
