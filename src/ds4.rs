//! Virtual DualShock 4 exposed by the winstadia UMDF driver (driver/winstadia.c).

use crate::stadia::State;

pub const VENDOR_ID: u16 = 0x054C;
pub const PRODUCT_ID: u16 = 0x09CC;
/// Serial number string that tells the virtual pad apart from a real DS4.
pub const SERIAL: &str = "winstadia";

// App channel feature reports, see the driver source.
const REPORT_ID_APP_INPUT: u8 = 0xE0;
const REPORT_ID_APP_OUTPUT: u8 = 0xE1;
pub const APP_REPORT_LEN: usize = 64;

// DS4 input report 0x01 layout (USB):
//   [1..5] left X, left Y, right X, right Y: centered at 128, Y grows downwards
//   [5] triangle, circle, cross, square (bit 7..4), hat (bit 3..0, 8 = released)
//   [6] R3, L3, Options, Share, R2, L2, R1, L1 (bit 7..0)
//   [7] report counter (bit 7..2), touchpad click, PS
//   [8..10] L2, R2 analog
//   [10..12] timestamp
//   [30] cable state and battery level
//   [35], [39] touch points, bit 7 set = not touching
const B5_TRIANGLE: u8 = 0x80;
const B5_CIRCLE: u8 = 0x40;
const B5_CROSS: u8 = 0x20;
const B5_SQUARE: u8 = 0x10;

const B6_R3: u8 = 0x80;
const B6_L3: u8 = 0x40;
const B6_OPTIONS: u8 = 0x20;
const B6_SHARE: u8 = 0x10;
const B6_R2: u8 = 0x08;
const B6_L2: u8 = 0x04;
const B6_R1: u8 = 0x02;
const B6_L1: u8 = 0x01;

const B7_TOUCHPAD: u8 = 0x02;
const B7_PS: u8 = 0x01;

const USB_POWERED_FULL: u8 = 0x1B;
const NOT_TOUCHING: u8 = 0x80;

// DS4 output report 0x05 layout: [1] flags, [4] weak motor, [5] strong motor.
const OUTPUT_FLAG_RUMBLE: u8 = 0x01;

/// Builds the app channel feature report that makes the driver publish
/// `state` as a DS4 input report. `counter` advances once per report.
pub fn input_feature_report(state: &State, counter: u8) -> [u8; APP_REPORT_LEN] {
    let mut report = [0u8; APP_REPORT_LEN];
    let b = &state.buttons;
    let bit = |pressed: bool, mask: u8| if pressed { mask } else { 0 };

    report[0] = REPORT_ID_APP_INPUT;
    (report[1], report[2]) = state.left;
    (report[3], report[4]) = state.right;
    report[5] = state.hat
        | bit(b.y, B5_TRIANGLE)
        | bit(b.b, B5_CIRCLE)
        | bit(b.a, B5_CROSS)
        | bit(b.x, B5_SQUARE);
    report[6] = bit(b.rs, B6_R3)
        | bit(b.ls, B6_L3)
        | bit(b.menu, B6_OPTIONS)
        | bit(b.options, B6_SHARE)
        | bit(state.r2 != 0, B6_R2)
        | bit(state.l2 != 0, B6_L2)
        | bit(b.r1, B6_R1)
        | bit(b.l1, B6_L1);
    report[7] = counter << 2 | bit(b.capture, B7_TOUCHPAD) | bit(b.stadia, B7_PS);
    report[8] = state.l2;
    report[9] = state.r2;
    // The real pad counts in 5.33 ms units at a 4 ms report interval.
    let timestamp = (counter as u16).wrapping_mul(188).to_le_bytes();
    (report[10], report[11]) = (timestamp[0], timestamp[1]);
    report[30] = USB_POWERED_FULL;
    report[35] = NOT_TOUCHING;
    report[39] = NOT_TOUCHING;
    report
}

/// Buffer for polling the output state, passed to `get_feature_report`.
pub fn output_feature_request() -> [u8; APP_REPORT_LEN] {
    let mut report = [0u8; APP_REPORT_LEN];
    report[0] = REPORT_ID_APP_OUTPUT;
    report
}

/// Output state polled from the driver: `[1]` is a sequence number that
/// advances on every output report written by a game, `[2..]` is that report
/// without its ID byte.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Output {
    pub sequence: u8,
    /// (strong, weak) motor speeds, if the report updates them.
    pub rumble: Option<(u8, u8)>,
}

pub fn parse_output_feature(report: &[u8]) -> Option<Output> {
    if report.len() < 7 || report[0] != REPORT_ID_APP_OUTPUT {
        return None;
    }
    let (flags, weak, strong) = (report[2], report[5], report[6]);
    Some(Output {
        sequence: report[1],
        rumble: (flags & OUTPUT_FLAG_RUMBLE != 0).then_some((strong, weak)),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stadia::Buttons;

    #[test]
    fn idle_report_matches_driver_idle_state() {
        let report = input_feature_report(&State::default(), 0);
        let mut expected = [0u8; APP_REPORT_LEN];
        expected[0] = 0xE0;
        expected[1..5].fill(0x80);
        expected[5] = 0x08;
        expected[30] = 0x1B;
        expected[35] = 0x80;
        expected[39] = 0x80;
        assert_eq!(report, expected);
    }

    #[test]
    fn maps_buttons_by_position() {
        let state = State {
            buttons: Buttons { a: true, l1: true, menu: true, stadia: true, ..Default::default() },
            hat: 2,
            l2: 0xFF,
            ..Default::default()
        };
        let report = input_feature_report(&state, 3);
        assert_eq!(report[5], B5_CROSS | 2);
        assert_eq!(report[6], B6_OPTIONS | B6_L2 | B6_L1);
        assert_eq!(report[7], 3 << 2 | B7_PS);
        assert_eq!((report[8], report[9]), (0xFF, 0));
    }

    #[test]
    fn parses_rumble_only_when_flagged() {
        let mut report = output_feature_request();
        report[1] = 7;
        report[5] = 10;
        report[6] = 200;
        assert_eq!(parse_output_feature(&report), Some(Output { sequence: 7, rumble: None }));
        report[2] = 0xF7;
        assert_eq!(
            parse_output_feature(&report),
            Some(Output { sequence: 7, rumble: Some((200, 10)) })
        );
    }
}

#[cfg(test)]
mod driver_tests {
    use super::*;
    use crate::stadia::Buttons;
    use hidapi::HidApi;

    #[test]
    #[ignore = "requires the installed driver"]
    fn input_roundtrip_through_driver() {
        let api = HidApi::new().unwrap();
        let info = api
            .device_list()
            .find(|d| d.vendor_id() == VENDOR_ID && d.serial_number() == Some(SERIAL))
            .expect("virtual pad not found");
        assert_eq!(info.product_id(), PRODUCT_ID);
        assert_eq!(info.product_string(), Some("Wireless Controller"));
        let pad = info.open_device(&api).unwrap();

        let state = State {
            buttons: Buttons { a: true, ..Default::default() },
            left: (0x10, 0xF0),
            ..Default::default()
        };
        let sent = input_feature_report(&state, 5);
        pad.send_feature_report(&sent).unwrap();

        let mut received = [0u8; APP_REPORT_LEN];
        let len = pad.read_timeout(&mut received, 1000).unwrap();
        assert_eq!(len, APP_REPORT_LEN);
        assert_eq!(received[0], 0x01);
        assert_eq!(received[1..], sent[1..]);

        // A game writing rumble shows up on the output channel.
        let mut rumble = [0u8; 32];
        rumble[..6].copy_from_slice(&[0x05, 0x01, 0, 0, 40, 90]);
        let before = pad_output(&pad);
        pad.write(&rumble).unwrap();
        let after = pad_output(&pad);
        assert_eq!(after.sequence, before.sequence.wrapping_add(1));
        assert_eq!(after.rumble, Some((90, 40)));

        pad.send_feature_report(&input_feature_report(&State::default(), 0)).unwrap();
    }

    fn pad_output(pad: &hidapi::HidDevice) -> Output {
        let mut request = output_feature_request();
        let len = pad.get_feature_report(&mut request).unwrap();
        parse_output_feature(&request[..len]).unwrap()
    }
}
