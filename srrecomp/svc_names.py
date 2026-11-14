"""Horizon (Switch 1) supervisor call names, as documented by public homebrew references (switchbrew).

IDs absent here are reported as svc_0xNN; Switch 2 firmware may define calls this table does not know.
"""
SVC = {
    0x01: 'SetHeapSize', 0x02: 'SetMemoryPermission', 0x03: 'SetMemoryAttribute', 0x04: 'MapMemory',
    0x05: 'UnmapMemory', 0x06: 'QueryMemory', 0x07: 'ExitProcess', 0x08: 'CreateThread',
    0x09: 'StartThread', 0x0A: 'ExitThread', 0x0B: 'SleepThread', 0x0C: 'GetThreadPriority',
    0x0D: 'SetThreadPriority', 0x0E: 'GetThreadCoreMask', 0x0F: 'SetThreadCoreMask',
    0x10: 'GetCurrentProcessorNumber', 0x11: 'SignalEvent', 0x12: 'ClearEvent', 0x13: 'MapSharedMemory',
    0x14: 'UnmapSharedMemory', 0x15: 'CreateTransferMemory', 0x16: 'CloseHandle', 0x17: 'ResetSignal',
    0x18: 'WaitSynchronization', 0x19: 'CancelSynchronization', 0x1A: 'ArbitrateLock',
    0x1B: 'ArbitrateUnlock', 0x1C: 'WaitProcessWideKeyAtomic', 0x1D: 'SignalProcessWideKey',
    0x1E: 'GetSystemTick', 0x1F: 'ConnectToNamedPort', 0x20: 'SendSyncRequestLight',
    0x21: 'SendSyncRequest', 0x22: 'SendSyncRequestWithUserBuffer', 0x23: 'SendAsyncRequestWithUserBuffer',
    0x24: 'GetProcessId', 0x25: 'GetThreadId', 0x26: 'Break', 0x27: 'OutputDebugString',
    0x28: 'ReturnFromException', 0x29: 'GetInfo', 0x2A: 'FlushEntireDataCache', 0x2B: 'FlushDataCache',
    0x2C: 'MapPhysicalMemory', 0x2D: 'UnmapPhysicalMemory', 0x2E: 'GetDebugFutureThreadInfo',
    0x2F: 'GetLastThreadInfo', 0x30: 'GetResourceLimitLimitValue', 0x31: 'GetResourceLimitCurrentValue',
    0x32: 'SetThreadActivity', 0x33: 'GetThreadContext3', 0x34: 'WaitForAddress', 0x35: 'SignalToAddress',
    0x36: 'SynchronizePreemptionState', 0x37: 'GetResourceLimitPeakValue', 0x39: 'CreateIoPool',
    0x3A: 'CreateIoRegion', 0x3C: 'KernelDebug', 0x3D: 'ChangeKernelTraceState',
    0x40: 'CreateSession', 0x41: 'AcceptSession', 0x42: 'ReplyAndReceiveLight', 0x43: 'ReplyAndReceive',
    0x44: 'ReplyAndReceiveWithUserBuffer', 0x45: 'CreateEvent', 0x46: 'MapIoRegion', 0x47: 'UnmapIoRegion',
    0x48: 'MapPhysicalMemoryUnsafe', 0x49: 'UnmapPhysicalMemoryUnsafe', 0x4A: 'SetUnsafeLimit',
    0x4B: 'CreateCodeMemory', 0x4C: 'ControlCodeMemory', 0x4D: 'SleepSystem', 0x4E: 'ReadWriteRegister',
    0x4F: 'SetProcessActivity', 0x50: 'CreateSharedMemory', 0x51: 'MapTransferMemory',
    0x52: 'UnmapTransferMemory', 0x53: 'CreateInterruptEvent', 0x54: 'QueryPhysicalAddress',
    0x55: 'QueryIoMapping', 0x56: 'CreateDeviceAddressSpace', 0x57: 'AttachDeviceAddressSpace',
    0x58: 'DetachDeviceAddressSpace', 0x59: 'MapDeviceAddressSpaceByForce',
    0x5A: 'MapDeviceAddressSpaceAligned', 0x5B: 'MapDeviceAddressSpace', 0x5C: 'UnmapDeviceAddressSpace',
    0x5D: 'InvalidateProcessDataCache', 0x5E: 'StoreProcessDataCache', 0x5F: 'FlushProcessDataCache',
    0x60: 'DebugActiveProcess', 0x61: 'BreakDebugProcess', 0x62: 'TerminateDebugProcess',
    0x63: 'GetDebugEvent', 0x64: 'ContinueDebugEvent', 0x65: 'GetProcessList', 0x66: 'GetThreadList',
    0x67: 'GetDebugThreadContext', 0x68: 'SetDebugThreadContext', 0x69: 'QueryDebugProcessMemory',
    0x6A: 'ReadDebugProcessMemory', 0x6B: 'WriteDebugProcessMemory', 0x6C: 'SetHardwareBreakPoint',
    0x6D: 'GetDebugThreadParam', 0x6F: 'GetSystemInfo', 0x70: 'CreatePort', 0x71: 'ManageNamedPort',
    0x72: 'ConnectToPort', 0x73: 'SetProcessMemoryPermission', 0x74: 'MapProcessMemory',
    0x75: 'UnmapProcessMemory', 0x76: 'QueryProcessMemory', 0x77: 'MapProcessCodeMemory',
    0x78: 'UnmapProcessCodeMemory', 0x79: 'CreateProcess', 0x7A: 'StartProcess', 0x7B: 'TerminateProcess',
    0x7C: 'GetProcessInfo', 0x7D: 'CreateResourceLimit', 0x7E: 'SetResourceLimitLimitValue',
    0x7F: 'CallSecureMonitor',
}


def name(i):
    return SVC.get(i, f'svc_{i:#04x}')
