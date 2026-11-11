import Foundation
@preconcurrency import CoreBluetooth

public enum NUSUUIDs {
    public static let serviceString = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    public static let rxString = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    public static let txString = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

    public static var service: CBUUID { CBUUID(string: serviceString) }
    public static var rx: CBUUID { CBUUID(string: rxString) }
    public static var tx: CBUUID { CBUUID(string: txString) }
}

public protocol NUSCentralDelegate: AnyObject {
    func nusCentral(_ central: NUSCentralManager, didUpdateState state: CBManagerState)
    func nusCentral(_ central: NUSCentralManager, didDiscover peripheral: CBPeripheral, rssi: NSNumber)
    func nusCentral(_ central: NUSCentralManager, didConnect peripheral: CBPeripheral)
    func nusCentralDidBecomeReady(_ central: NUSCentralManager)
    func nusCentral(_ central: NUSCentralManager, didDisconnect peripheral: CBPeripheral, error: Error?)
    func nusCentral(_ central: NUSCentralManager, didReceive message: NUSInboundMessage, from peripheral: CBPeripheral)
    func nusCentral(_ central: NUSCentralManager, didFailWith error: Error)
}

public final class NUSCentralManager: NSObject {
    public weak var delegate: NUSCentralDelegate?

    /// System uses this to relaunch / restore the central after background suspension.
    public static let restoreIdentifier = "macboard.ble.nus.central"

    private let namePrefix: String
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var txCharacteristic: CBCharacteristic?
    private var rxCharacteristic: CBCharacteristic?
    private var framer = NDJSONLineFramer()
    private var wantsScan = false
    private var isReady = false

    private struct PendingWrite {
        var data: Data
        var offset: Int
        let completion: ((Error?) -> Void)?
    }

    /// Serialise BLE writes so withoutResponse / multi-chunk lines are not dropped.
    private var writeQueue: [PendingWrite] = []
    private var writeInFlight = false

    public init(namePrefix: String = "Claude") {
        self.namePrefix = namePrefix
        super.init()
        self.manager = CBCentralManager(
            delegate: self,
            queue: nil,
            options: [
                CBCentralManagerOptionRestoreIdentifierKey: Self.restoreIdentifier,
                CBCentralManagerOptionShowPowerAlertKey: true,
            ]
        )
    }

    public func startScanning() {
        wantsScan = true
        guard manager.state == .poweredOn else { return }
        beginScanning()
    }

    public func stopScanning() {
        wantsScan = false
        manager.stopScan()
    }

    public func connect(_ peripheral: CBPeripheral) {
        manager.stopScan()
        wantsScan = false
        resetLinkState(keepPeripheral: false)
        self.peripheral = peripheral
        peripheral.delegate = self
        manager.connect(
            peripheral,
            options: [
                CBConnectPeripheralOptionNotifyOnConnectionKey: true,
                CBConnectPeripheralOptionNotifyOnDisconnectionKey: true,
                CBConnectPeripheralOptionNotifyOnNotificationKey: true,
            ]
        )
    }

    @discardableResult
    public func retrieveAndConnect(uuid: UUID) -> Bool {
        let found = manager.retrievePeripherals(withIdentifiers: [uuid])
        guard let target = found.first else { return false }
        connect(target)
        return true
    }

    public func disconnect() {
        guard let peripheral else { return }
        manager.cancelPeripheralConnection(peripheral)
    }

    public func sendHeartbeat(_ snapshot: HeartbeatSnapshot, completion: ((Error?) -> Void)? = nil) {
        do {
            let data = try NDJSON.encodeLine(snapshot)
            write(data, completion: completion)
        } catch {
            completion?(error)
        }
    }

    public func send(_ outbound: NUSOutbound, completion: ((Error?) -> Void)? = nil) {
        switch outbound {
        case .rawJSON(let data):
            var line = data
            if line.last != 0x0A {
                line.append(0x0A)
            }
            write(line, completion: completion)
        }
    }

    private func beginScanning() {
        manager.scanForPeripherals(withServices: [NUSUUIDs.service], options: nil)
    }

    private func write(_ data: Data, completion: ((Error?) -> Void)?) {
        guard isReady, rxCharacteristic != nil, peripheral != nil else {
            completion?(MacBoardBLEError.notReady)
            return
        }
        writeQueue.append(PendingWrite(data: data, offset: 0, completion: completion))
        pumpWriteQueue()
    }

    private func preferredWriteType(for rx: CBCharacteristic) -> CBCharacteristicWriteType {
        // Prefer acknowledged writes so heartbeats / custom text are not silently dropped.
        if rx.properties.contains(.write) {
            return .withResponse
        }
        return .withoutResponse
    }

    private func pumpWriteQueue() {
        guard !writeInFlight else { return }
        guard isReady, let peripheral, let rx = rxCharacteristic else {
            failAllWrites(MacBoardBLEError.notReady)
            return
        }
        guard !writeQueue.isEmpty else { return }

        let type = preferredWriteType(for: rx)
        if type == .withoutResponse, !peripheral.canSendWriteWithoutResponse {
            return
        }

        var item = writeQueue[0]
        let mtu = max(20, peripheral.maximumWriteValueLength(for: type))
        let count = min(mtu, item.data.count - item.offset)
        let chunk = item.data.subdata(in: item.offset..<(item.offset + count))
        writeInFlight = true
        peripheral.writeValue(chunk, for: rx, type: type)
        item.offset += count
        writeQueue[0] = item

        if type == .withoutResponse {
            // withoutResponse: advance immediately; further chunks wait for isReady callback.
            finishCurrentChunk(error: nil)
        }
        // withResponse: wait for didWriteValueFor
    }

    private func finishCurrentChunk(error: Error?) {
        writeInFlight = false
        guard !writeQueue.isEmpty else { return }

        if let error {
            let completion = writeQueue.removeFirst().completion
            completion?(error)
            // Continue remaining queue so one failure does not wedge forever.
            pumpWriteQueue()
            return
        }

        if writeQueue[0].offset >= writeQueue[0].data.count {
            let completion = writeQueue.removeFirst().completion
            completion?(nil)
        }
        pumpWriteQueue()
    }

    private func failAllWrites(_ error: Error) {
        writeInFlight = false
        let pending = writeQueue
        writeQueue.removeAll()
        for item in pending {
            item.completion?(error)
        }
    }

    private func resetLinkState(keepPeripheral: Bool) {
        isReady = false
        txCharacteristic = nil
        rxCharacteristic = nil
        framer.reset()
        failAllWrites(MacBoardBLEError.notReady)
        if !keepPeripheral {
            peripheral?.delegate = nil
            peripheral = nil
        }
    }
}

extension NUSCentralManager: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        delegate?.nusCentral(self, didUpdateState: central.state)
        if central.state == .poweredOn, wantsScan {
            beginScanning()
        }
        if central.state == .poweredOff || central.state == .unauthorized {
            resetLinkState(keepPeripheral: false)
        }
    }

    public func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let peripherals = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral],
           let restored = peripherals.first {
            peripheral = restored
            restored.delegate = self
            switch restored.state {
            case .connected:
                delegate?.nusCentral(self, didConnect: restored)
                restored.discoverServices([NUSUUIDs.service])
            default:
                break
            }
        }
        if let services = dict[CBCentralManagerRestoredStateScanServicesKey] as? [CBUUID], !services.isEmpty {
            wantsScan = true
        }
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName = (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? peripheral.name
            ?? ""
        guard advertisedName.lowercased().hasPrefix(namePrefix.lowercased()) else { return }
        delegate?.nusCentral(self, didDiscover: peripheral, rssi: RSSI)
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        delegate?.nusCentral(self, didConnect: peripheral)
        peripheral.discoverServices([NUSUUIDs.service])
    }

    public func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        resetLinkState(keepPeripheral: false)
        if let error {
            delegate?.nusCentral(self, didFailWith: error)
        } else {
            delegate?.nusCentral(self, didFailWith: MacBoardBLEError.notReady)
        }
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        resetLinkState(keepPeripheral: false)
        delegate?.nusCentral(self, didDisconnect: peripheral, error: error)
    }
}

extension NUSCentralManager: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            delegate?.nusCentral(self, didFailWith: error)
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == NUSUUIDs.service }) else {
            delegate?.nusCentral(self, didFailWith: MacBoardBLEError.notReady)
            return
        }
        peripheral.discoverCharacteristics([NUSUUIDs.tx, NUSUUIDs.rx], for: service)
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            delegate?.nusCentral(self, didFailWith: error)
            return
        }
        for characteristic in service.characteristics ?? [] {
            if characteristic.uuid == NUSUUIDs.tx {
                txCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            } else if characteristic.uuid == NUSUUIDs.rx {
                rxCharacteristic = characteristic
            }
        }
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            delegate?.nusCentral(self, didFailWith: error)
            return
        }
        guard characteristic.uuid == NUSUUIDs.tx, characteristic.isNotifying else { return }
        if rxCharacteristic != nil {
            isReady = true
            delegate?.nusCentralDidBecomeReady(self)
            pumpWriteQueue()
        }
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == NUSUUIDs.rx else { return }
        finishCurrentChunk(error: error)
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pumpWriteQueue()
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            delegate?.nusCentral(self, didFailWith: error)
            return
        }
        guard characteristic.uuid == NUSUUIDs.tx, let value = characteristic.value else { return }
        let lines = framer.ingest(value)
        for line in lines {
            let message = NUSInboundDecoder.decode(line)
            delegate?.nusCentral(self, didReceive: message, from: peripheral)
        }
    }
}
