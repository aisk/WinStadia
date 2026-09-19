mod controller;
mod discovery;
mod ds4;
mod stadia;

use std::error::Error;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use hidapi::{HidApi, HidDevice};

use controller::Controller;

const READ_TIMEOUT_MS: i32 = 8;
const OUTPUT_POLL_INTERVAL: Duration = Duration::from_millis(16);
const RESCAN_INTERVAL: Duration = Duration::from_secs(1);

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn open_controller(api: &HidApi) -> Option<Controller> {
    discovery::controller_open_paths()
        .iter()
        .find_map(|path| Controller::open(api, path))
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

fn poll_output(pad: &HidDevice) -> Result<Option<ds4::Output>> {
    let mut request = ds4::output_feature_request();
    let len = pad.get_feature_report(&mut request)?;
    Ok(ds4::parse_output_feature(&request[..len]))
}

/// Bridges the controller to the virtual pad until the controller
/// disconnects or `stop` is set.
fn run_session(controller: &Controller, pad: &HidDevice, stop: &AtomicBool) -> Result<()> {
    let mut counter = 0u8;
    let mut last_state = None;
    // The driver keeps the last output report around; only rumble written
    // during this session counts.
    let mut output_sequence = poll_output(pad)?.map(|output| output.sequence);
    let mut last_output_poll = Instant::now();
    let mut rumble_failed = false;
    let mut report = [0u8; 64];

    while !stop.load(Ordering::Relaxed) {
        if last_output_poll.elapsed() >= OUTPUT_POLL_INTERVAL {
            last_output_poll = Instant::now();
            if let Some(output) = poll_output(pad)?
                && output_sequence.replace(output.sequence) != Some(output.sequence)
                && let Some((strong, weak)) = output.rumble
                && let Err(e) = controller.rumble(strong, weak)
                && !rumble_failed
            {
                // A lost controller shows up as a read error below.
                println!("Rumble is not working: {e}");
                rumble_failed = true;
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
        let _ = controller.rumble(0, 0);
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

    let mut api = HidApi::new()?;
    match std::env::args().nth(1).as_deref() {
        None => bridge(&mut api, &stop),
        Some("dump") => dump(&mut api, &stop),
        Some("rumble") => rumble(&api, &stop),
        Some(_) => Err("usage: winstadia [dump | rumble]".into()),
    }
}
