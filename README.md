# Custom Pomodoro Timer (Hardware)

A standalone Pomodoro study timer built on an Arduino Uno R3, programmed in C++, with a 16x2 LCD, four push buttons, and a buzzer. It works on its own with its buttons, or it can be controlled from a computer over USB through **Pomegranate**, the companion web app (see the [app README](../README.md)). The timer is housed in a custom 3D-printed case designed in Autodesk Fusion, with a magnet-secured two-piece design for easy access to the electronics.

## Features
- **Auto mode:** classic 25 min focus / 5 min break, with a prompt to continue after each cycle
- **Custom mode:** choose focus length, break length, and number of cycles
- **Pause, resume, or exit** any running timer
- **Mute button**, saved in EEPROM so it's remembered after a restart
- Checkpoint and summary screens tracking completed sessions and total focus time
- **USB control:** every action can also be sent as a text command over serial, so the timer and app always stay in sync

## Parts
- Arduino Uno R3
- 16x2 character LCD (LiquidCrystal library)
- 4 push buttons (pins 6, 8, 9, 10)
- Piezo buzzer (pin 7)
- 3D-printed enclosure (Autodesk Fusion)

## Controls
| Button | Menus | While a timer is running |
|---|---|---|
| Pin 9 | Next / increase | — |
| Pin 8 | Previous / decrease | — |
| Pin 10 | Select | Pause (then Resume or Exit) |
| Pin 6 | Mute / unmute | Mute / unmute |

## Uploading the firmware
Open `Pomodoro_Timer_Code/` in the Arduino IDE, select **Arduino Uno** as the board, and upload. Close the IDE's Serial Monitor before using the app, since only one program can use the USB port at a time.

## How it works
The firmware is a non-blocking state machine: timing uses `millis()` instead of `delay()`, so the buttons and USB commands are both handled at any moment, and the LCD and app always stay in sync.

### Serial protocol (115200 baud, one line per message)
| Computer → timer | Meaning |
|---|---|
| `START f b c` | Custom session: `f` min focus, `b` min break, `c` cycles |
| `AUTO` | Auto mode (25/5, asks to continue) |
| `PAUSE` / `RESUME` / `EXIT` | Control the running timer |
| `YES` / `NO` | Answer the continue prompt |
| `MUTE` / `UNMUTE` | Turn the buzzer off / on |
| `STATUS` | Request the current state |

The timer replies with `STATE ...` lines (every second and on every change), `EVENT ...` lines when a timer ends, and `ERR ...` if a command can't be done.

## Folder structure
```
Pomodoro_Timer_Code/   Arduino sketch (open in the Arduino IDE)
```
