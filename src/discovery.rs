//! Finds Stadia controllers without opening them.
//!
//! hidapi enumerates by opening every HID device, which the hide filter
//! (driver/hide.c) denies. The interface path alone identifies the controller,
//! and appending `OPEN_SUFFIX` to it gets an open past the filter.

use std::ffi::CString;
use std::ptr;

use windows_sys::Win32::Devices::DeviceAndDriverInstallation::{
    CM_GET_DEVICE_INTERFACE_LIST_PRESENT, CM_Get_Device_Interface_List_SizeW,
    CM_Get_Device_Interface_ListW, CR_BUFFER_SMALL, CR_SUCCESS,
};
use windows_sys::Win32::Devices::HumanInterfaceDevice::GUID_DEVINTERFACE_HID;

const OPEN_SUFFIX: &str = "\\winstadia";

// Hardware ID fragments in the interface path, for USB and Bluetooth LE.
const PATH_MARKERS: [&str; 2] = ["vid_18d1&pid_9400", "vid&0218d1_pid&9400"];

fn hid_interface_paths() -> Vec<String> {
    loop {
        let mut len = 0u32;
        let mut buffer;
        // The list can grow between the two calls, hence the retry.
        let result = unsafe {
            if CM_Get_Device_Interface_List_SizeW(
                &mut len,
                &GUID_DEVINTERFACE_HID,
                ptr::null(),
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT,
            ) != CR_SUCCESS
            {
                return Vec::new();
            }
            buffer = vec![0u16; len as usize];
            CM_Get_Device_Interface_ListW(
                &GUID_DEVINTERFACE_HID,
                ptr::null(),
                buffer.as_mut_ptr(),
                len,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT,
            )
        };
        match result {
            CR_SUCCESS => {
                return buffer
                    .split(|&c| c == 0)
                    .filter(|path| !path.is_empty())
                    .map(String::from_utf16_lossy)
                    .collect();
            }
            CR_BUFFER_SMALL => continue,
            _ => return Vec::new(),
        }
    }
}

fn is_controller_path(path: &str) -> bool {
    let path = path.to_ascii_lowercase();
    PATH_MARKERS.iter().any(|marker| path.contains(marker))
}

/// Paths to pass to `HidApi::open_path` for every connected controller.
pub fn controller_open_paths() -> Vec<CString> {
    hid_interface_paths()
        .into_iter()
        .filter(|path| is_controller_path(path))
        .filter_map(|path| CString::new(path + OPEN_SUFFIX).ok())
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recognizes_usb_and_bluetooth_paths() {
        assert!(is_controller_path(
            r"\\?\HID#VID_18D1&PID_9400&MI_01#8&299effdf&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"
        ));
        assert!(is_controller_path(
            r"\\?\HID#{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&0218d1_PID&9400_REV&0100_fdc369032506#a&1a848439&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"
        ));
        assert!(!is_controller_path(
            r"\\?\HID#VID_054C&PID_09CC#1&2d595ca7&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"
        ));
    }
}
