//! An opened Stadia controller.

use std::cell::Cell;
use std::ffi::CStr;
use std::io;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle};
use std::ptr;

use hidapi::{HidApi, HidDevice, HidResult};
use windows_sys::Win32::Devices::HumanInterfaceDevice::HidD_SetOutputReport;
use windows_sys::Win32::Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::Storage::FileSystem::{
    CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
};

use crate::stadia;

pub struct Controller {
    device: HidDevice,
    // hidapi's send_output_report sizes its buffer by the feature report
    // length, which is zero for this device, so SetOutputReport needs a
    // handle of our own.
    output: OwnedHandle,
    // WriteFile works over USB but fails with ERROR_INVALID_PARAMETER over
    // Bluetooth LE, where only SetOutputReport gets through.
    write_file_fails: Cell<bool>,
}

impl Controller {
    pub fn open(api: &HidApi, path: &CStr) -> Option<Controller> {
        let device = api.open_path(path).ok()?;
        let wide: Vec<u16> = path.to_str().ok()?.encode_utf16().chain([0]).collect();
        let handle = unsafe {
            CreateFileW(
                wide.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                ptr::null(),
                OPEN_EXISTING,
                0,
                ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            return None;
        }
        Some(Controller {
            device,
            output: unsafe { OwnedHandle::from_raw_handle(handle) },
            write_file_fails: Cell::new(false),
        })
    }

    pub fn read_timeout(&self, report: &mut [u8], timeout_ms: i32) -> HidResult<usize> {
        self.device.read_timeout(report, timeout_ms)
    }

    pub fn rumble(&self, strong: u8, weak: u8) -> io::Result<()> {
        let mut report = stadia::rumble_report(strong, weak);
        if !self.write_file_fails.get() {
            if self.device.write(&report).is_ok() {
                return Ok(());
            }
            self.write_file_fails.set(true);
        }
        let sent = unsafe {
            HidD_SetOutputReport(
                self.output.as_raw_handle(),
                report.as_mut_ptr().cast(),
                report.len() as u32,
            )
        };
        if sent { Ok(()) } else { Err(io::Error::last_os_error()) }
    }
}
