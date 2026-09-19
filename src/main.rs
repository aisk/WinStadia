//! Diagnostics for the winstadia driver. The driver works on its own; this
//! tool only asks it what it is doing.

mod pad;
mod stadia;

use std::error::Error;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::Duration;

use hidapi::{HidApi, HidDevice};

const POLL_INTERVAL: Duration = Duration::from_millis(4);

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn open_pad(api: &HidApi) -> Result<HidDevice> {
    let info = api
        .device_list()
        .find(|d| {
            d.vendor_id() == pad::VENDOR_ID
                && d.product_id() == pad::PRODUCT_ID
                && d.serial_number() == Some(pad::SERIAL)
        })
        .ok_or("winstadia DualShock 4 not found, is the controller plugged in and the driver installed?")?;
    Ok(info.open_device(api)?)
}

fn read_status(device: &HidDevice) -> Result<pad::Status> {
    let mut request = pad::status_request();
    let len = device.get_feature_report(&mut request)?;
    Ok(pad::parse_status(&request[..len]).ok_or("unexpected status report")?)
}

/// Reports what the driver is doing.
fn status(api: &HidApi) -> Result<()> {
    println!("Driver: {}", read_status(&open_pad(api)?)?);
    Ok(())
}

/// Prints raw Stadia input reports as they change, to verify the report layout.
fn dump(api: &HidApi, stop: &AtomicBool) -> Result<()> {
    let device = open_pad(api)?;
    println!("Dumping input reports, press Ctrl+C to stop.");

    let mut last = Vec::new();
    while !stop.load(Ordering::Relaxed) {
        let report = read_status(&device)?.raw_report;
        if !report.is_empty() && report != last {
            let hex: Vec<_> = report.iter().map(|b| format!("{b:02X}")).collect();
            println!("{}  {:?}", hex.join(" "), stadia::parse(&report));
            last = report;
        }
        thread::sleep(POLL_INTERVAL);
    }
    Ok(())
}

/// Pulses both motors, to check the rumble path to the controller.
fn rumble(api: &HidApi) -> Result<()> {
    let device = open_pad(api)?;
    device.write(&pad::rumble_report(255, 255))?;
    thread::sleep(Duration::from_millis(500));
    device.write(&pad::rumble_report(0, 0))?;
    println!("Rumble sent. Driver: {}", read_status(&device)?);
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
        Some("rumble") => rumble(&api),
        Some(_) => Err("usage: winstadia [status | dump | rumble]".into()),
    }
}
