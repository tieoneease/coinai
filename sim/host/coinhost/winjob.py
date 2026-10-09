"""Windows job object: child processes die with the host, even if the host is killed."""

from __future__ import annotations

import logging
import sys

log = logging.getLogger('coinsim')


class JobObject:
    def __init__(self) -> None:
        self.handle = None
        if sys.platform != 'win32':
            return
        try:
            import ctypes
            from ctypes import wintypes as wt

            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.CreateJobObjectW.restype = wt.HANDLE
            k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, wt.LPCWSTR]
            k32.SetInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD]
            k32.OpenProcess.restype = wt.HANDLE
            k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
            k32.AssignProcessToJobObject.argtypes = [wt.HANDLE, wt.HANDLE]
            k32.CloseHandle.argtypes = [wt.HANDLE]

            class IoCounters(ctypes.Structure):
                _fields_ = [(n, ctypes.c_ulonglong) for n in (
                    'ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                    'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]

            class Basic(ctypes.Structure):
                _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                            ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                            ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                            ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD)]

            class Extended(ctypes.Structure):
                _fields_ = [('BasicLimitInformation', Basic), ('IoInfo', IoCounters),
                            ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                            ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]

            handle = k32.CreateJobObjectW(None, None)
            if not handle:
                return
            info = Extended()
            info.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            if not k32.SetInformationJobObject(handle, 9, ctypes.byref(info), ctypes.sizeof(info)):
                k32.CloseHandle(handle)
                return
            self._k32 = k32
            self.handle = handle
        except Exception as e:  # noqa: BLE001
            log.debug('job object unavailable: %s', e)

    def assign(self, pid: int) -> None:
        if not self.handle:
            return
        h = self._k32.OpenProcess(0x0100 | 0x0001, False, pid)  # PROCESS_SET_QUOTA | PROCESS_TERMINATE
        if h:
            self._k32.AssignProcessToJobObject(self.handle, h)
            self._k32.CloseHandle(h)
