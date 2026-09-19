//! Diagnostics for the winstadia drivers. The drivers work on their own; this
//! tool only inspects them and the physical controller.

mod controller;
mod discovery;
mod pad;
mod stadia;

use std::error::Error;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

use hidapi::HidApi;

use controller::Controller;

const READ_TIMEOUT_MS: i32 = 8;
const RESCAN_INTERVAL: Duration = Duration::from_secs(1);

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn open_controller(api: &HidApi) -> Option<Controller> {
    discovery::controller_open_paths()
        .iter()
        .find_map(|path| Controller::open(api, path))
}

fn wait_for_controller(api: &HidApi, stop: &AtomicBool) -> Option<Controller> {
    let mut announced = false;
    while !stop.load(Ordering::Relaxed) {
        if let Some(device) = open_controller(api) {
            return Some(device);
        }
        if !announced {
            println!("Waiting for a Stadia controller (USB or Bluetooth)...");
            announced = true;
        }
        thread::sleep(RESCAN_INTERVAL);
    }
    None
}

/// Reports what the driver's bridge to the controller is doing.
fn status(api: &HidApi) -> Result<()> {
    let info = api
        .device_list()
        .find(|d| {
            d.vendor_id() == pad::VENDOR_ID
                && d.product_id() == pad::PRODUCT_ID
                && d.serial_number() == Some(pad::SERIAL)
        })
        .ok_or("virtual DualShock 4 not found, install the driver with driver\\install.ps1")?;
    let device = info.open_device(api)?;

    let mut request = pad::status_request();
    let len = device.get_feature_report(&mut request)?;
    let status = pad::parse_status(&request[..len]).ok_or("unexpected status report")?;
    println!("Driver: {status}");
    Ok(())
}

/// Prints raw input reports as they change, to verify the report layout.
fn dump(api: &HidApi, stop: &AtomicBool) -> Result<()> {
    let Some(device) = wait_for_controller(api, stop) else {
        return Ok(());
    };
    println!("Dumping input reports, press Ctrl+C to stop.");

    let mut last = Vec::new();
    let mut report = [0u8; 64];
    while !stop.load(Ordering::Relaxed) {
        let len = device.read_timeout(&mut report, READ_TIMEOUT_MS)?;
        if len == 0 || last == report[..len] {
            continue;
        }
        last = report[..len].to_vec();
        let hex: Vec<_> = last.iter().map(|b| format!("{b:02X}")).collect();
        println!("{}  {:?}", hex.join(" "), stadia::parse(&last));
    }
    Ok(())
}

/// Pulses both motors, to check the rumble path to the controller.
fn rumble(api: &HidApi, stop: &AtomicBool) -> Result<()> {
    let Some(controller) = wait_for_controller(api, stop) else {
        return Ok(());
    };
    controller.rumble(255, 255)?;
    thread::sleep(Duration::from_millis(500));
    controller.rumble(0, 0)?;
    println!("Rumble sent.");
    Ok(())
}

fn main() -> Result<()> {
    let stop = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let stop = stop.clone();
        move || stop.store(true, Ordering::Relaxed)
    })?;

    let api = HidApi::new()?;
    match std::env::args().nth(1).as_deref() {
        None | Some("status") => status(&api),
        Some("dump") => dump(&api, &stop),
        Some("rumble") => rumble(&api, &stop),
        Some(_) => Err("usage: winstadia [status | dump | rumble]".into()),
    }
}
