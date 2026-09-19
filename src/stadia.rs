//! Stadia controller HID protocol (Bluetooth-mode firmware). The driver relays
//! raw input reports for diagnostics.

const INPUT_REPORT_ID: u8 = 0x03;

/// Minimum length of an input report, including the report ID byte.
pub const INPUT_REPORT_LEN: usize = 10;

// Input report layout:
//   [0] report ID (0x03)
//   [1] d-pad hat: 0 = up, clockwise to 7 = up-left, 8 = released
//   [2] RS click, Options, Menu, Stadia, R2, L2, Assistant, Capture (bit 7..0)
//   [3] -, A, B, X, Y, L1, R1, LS click (bit 7..0)
//   [4..8] left X, left Y, right X, right Y: 1..=255, centered at 128, Y grows downwards
//   [8..10] L2, R2 analog: 0..=255
//   [10] consumer keys (volume, play/pause), unused
const B2_RS: u8 = 0x80;
const B2_OPTIONS: u8 = 0x40;
const B2_MENU: u8 = 0x20;
const B2_STADIA: u8 = 0x10;
const B2_CAPTURE: u8 = 0x01;

const B3_A: u8 = 0x40;
const B3_B: u8 = 0x20;
const B3_X: u8 = 0x10;
const B3_Y: u8 = 0x08;
const B3_L1: u8 = 0x04;
const B3_R1: u8 = 0x02;
const B3_LS: u8 = 0x01;

pub const HAT_RELEASED: u8 = 8;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Buttons {
    pub a: bool,
    pub b: bool,
    pub x: bool,
    pub y: bool,
    pub l1: bool,
    pub r1: bool,
    pub ls: bool,
    pub rs: bool,
    pub options: bool,
    pub menu: bool,
    pub stadia: bool,
    pub capture: bool,
}

/// Controller state in the controller's own conventions: hat as in the input
/// report, sticks centered at 128 with Y growing downwards.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct State {
    pub buttons: Buttons,
    pub hat: u8,
    pub left: (u8, u8),
    pub right: (u8, u8),
    pub l2: u8,
    pub r2: u8,
}

impl Default for State {
    fn default() -> Self {
        State {
            buttons: Buttons::default(),
            hat: HAT_RELEASED,
            left: (0x80, 0x80),
            right: (0x80, 0x80),
            l2: 0,
            r2: 0,
        }
    }
}

/// Decodes a raw input report. Returns `None` for reports that are not
/// gamepad input reports.
pub fn parse(report: &[u8]) -> Option<State> {
    if report.len() < INPUT_REPORT_LEN || report[0] != INPUT_REPORT_ID {
        return None;
    }
    let (b2, b3) = (report[2], report[3]);
    Some(State {
        buttons: Buttons {
            a: b3 & B3_A != 0,
            b: b3 & B3_B != 0,
            x: b3 & B3_X != 0,
            y: b3 & B3_Y != 0,
            l1: b3 & B3_L1 != 0,
            r1: b3 & B3_R1 != 0,
            ls: b3 & B3_LS != 0,
            rs: b2 & B2_RS != 0,
            options: b2 & B2_OPTIONS != 0,
            menu: b2 & B2_MENU != 0,
            stadia: b2 & B2_STADIA != 0,
            capture: b2 & B2_CAPTURE != 0,
        },
        hat: report[1].min(HAT_RELEASED),
        left: (report[4], report[5]),
        right: (report[6], report[7]),
        l2: report[8],
        r2: report[9],
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const NEUTRAL: [u8; 11] = [0x03, 0x08, 0, 0, 0x80, 0x80, 0x80, 0x80, 0, 0, 0];

    #[test]
    fn neutral_report_is_default_state() {
        assert_eq!(parse(&NEUTRAL), Some(State::default()));
    }

    #[test]
    fn rejects_foreign_reports() {
        assert_eq!(parse(&[0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0]), None);
        assert_eq!(parse(&NEUTRAL[..9]), None);
    }

    // Reports captured from a real controller.
    #[test]
    fn decodes_captured_reports() {
        let a = parse(&[0x03, 0x08, 0x00, 0x40, 0x80, 0x80, 0x80, 0x80, 0, 0, 0]).unwrap();
        assert!(a.buttons.a);
        let l1 = parse(&[0x03, 0x08, 0x00, 0x04, 0x80, 0x80, 0x80, 0x80, 0, 0, 0]).unwrap();
        assert!(l1.buttons.l1);
        let up = parse(&[0x03, 0x00, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0, 0, 0]).unwrap();
        assert_eq!(up.hat, 0);
        let stick = parse(&[0x03, 0x08, 0x00, 0x00, 0x29, 0x92, 0x80, 0x80, 0, 0, 0]).unwrap();
        assert_eq!(stick.left, (0x29, 0x92));
    }
}
