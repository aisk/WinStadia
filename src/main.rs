mod ds4;
mod stadia;

use std::error::Error;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use hidapi::{HidApi, HidDevice};

const USAGE_PAGE_GENERIC_DESKTOP: u16 = 0x01;
const USAGE_GAMEPAD: u16 = 0x05;

const READ_TIMEOUT_MS: i32 = 8;
const OUTPUT_POLL_INTERVAL: Duration = Duration::from_millis(16);
const RESCAN_INTERVAL: Duration = Duration::from_secs(1);

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn open_controller(api: &mut HidApi) -> Option<HidDevice> {
    api.refresh_devices().ok()?;
    let mut candidates: Vec<_> = api
        .device_list()
        .filter(|d| d.vendor_id() == stadia::VENDOR_ID && d.product_id() == stadia::PRODUCT_ID)
        .collect();
    // Prefer the gamepad top-level collection if the device exposes several.
    candidates.sort_by_key(|d| {
        !(d.usage_page() == USAGE_PAGE_GENERIC_DESKTOP && d.usage() == USAGE_GAMEPAD)
    });
    candidates.into_iter().find_map(|d| d.open_device(api).ok())
}

fn open_virtual_pad(api: &mut HidApi) -> Result<HidDevice> {
    api.refresh_devices()?;
    let info = api
        .device_list()
        .find(|d| {
            d.vendor_id() == ds4::VENDOR_ID
                && d.product_id() == ds4::PRODUCT_ID
                && d.serial_number() == Some(ds4::SERIAL)
        })
        .ok_or("virtual DualShock 4 not found, install the driver with driver\\install.ps1")?;
    Ok(info.open_device(api)?)
}

fn wait_for_controller(api: &mut HidApi, stop: &AtomicBool) -> Option<HidDevice> {
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

/// Bridges the controller to the virtual pad until the controller
/// disconnects or `stop` is set.
fn run_session(controller: &HidDevice, pad: &HidDevice, stop: &AtomicBool) -> Result<()> {
    let mut counter = 0u8;
    let mut last_state = None;
    let mut output_sequence = None;
    let mut last_output_poll = Instant::now();
    let mut report = [0u8; 64];

    while !stop.load(Ordering::Relaxed) {
        if last_output_poll.elapsed() >= OUTPUT_POLL_INTERVAL {
            last_output_poll = Instant::now();
            let mut request = ds4::output_feature_request();
            let len = pad.get_feature_report(&mut request)?;
            if let Some(output) = ds4::parse_output_feature(&request[..len])
                && output_sequence.replace(output.sequence) != Some(output.sequence)
                && let Some((strong, weak)) = output.rumble
            {
                controller.write(&stadia::rumble_report(strong, weak))?;
            }
        }

        let len = controller.read_timeout(&mut report, READ_TIMEOUT_MS)?;
        let Some(state) = stadia::parse(&report[..len]) else {
            continue;
        };
        if last_state != Some(state) {
            pad.send_feature_report(&ds4::input_feature_report(&state, counter))?;
            counter = counter.wrapping_add(1);
            last_state = Some(state);
        }
    }
    Ok(())
}

fn bridge(api: &mut HidApi, stop: &AtomicBool) -> Result<()> {
    let pad = open_virtual_pad(api)?;

    while let Some(controller) = wait_for_controller(api, stop) {
        println!("Stadia controller connected.");
        let result = run_session(&controller, &pad, stop);
        let _ = controller.write(&stadia::rumble_report(0, 0));
        pad.send_feature_report(&ds4::input_feature_report(&stadia::State::default(), 0))?;
        if let Err(e) = result {
            println!("Controller disconnected: {e}");
        }
    }
    Ok(())
}

/// Prints raw input reports as they change, to verify the report layout.
fn dump(api: &mut HidApi, stop: &AtomicBool) -> Result<()> {
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

fn main() -> Result<()> {
    let stop = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let stop = stop.clone();
        move || stop.store(true, Ordering::Relaxed)
    })?;

    let mut api = HidApi::new()?;
    match std::env::args().nth(1).as_deref() {
        None => bridge(&mut api, &stop),
        Some("dump") => dump(&mut api, &stop),
        Some(_) => Err("usage: winstadia [dump]".into()),
    }
}
