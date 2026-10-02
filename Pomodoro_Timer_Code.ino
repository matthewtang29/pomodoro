/*
  Custom Pomodoro Timer - Matthew Tang
  Arduino Uno R3 + 16x2 LCD + 4 buttons + buzzer

  The timer can be controlled with the physical buttons OR from a computer
  over USB (see the web controller, index.html).

  How it works:
  The program is a "state machine". loop() runs thousands of times a second and
  never waits with delay(). Each pass it reads the buttons, reads USB messages,
  checks the clock with millis(), and updates the screen. That's what lets the
  buttons and the computer both work at any time, even mid-countdown.

  ---------------------------- USB PROTOCOL (115200 baud) ----------------------------
  Computer -> timer (one command per line):
    START f b c   start a custom session: f min focus, b min break, c cycles
    AUTO          start auto mode (25/5, asks to continue after each cycle)
    PAUSE         pause the running timer
    RESUME        resume a paused timer
    EXIT          end the session early
    YES / NO      answer the "CONTINUE ?" prompt in auto mode
    MUTE / UNMUTE turn the buzzer off / on (remembered after a restart)
    STATUS        ask for a STATE line right now

  Timer -> computer:
    READY                       sent once after start-up
    STATE state=FOCUS left=1432 cycle=1 cycles=4 focus=25 break=5 done=0 total=0 mode=CUSTOM muted=0
                                sent every second and whenever something changes
                                (left = seconds left, cycles=0 means unlimited/auto)
    EVENT FOCUS_DONE | REST_DONE | SESSION_ENDED | EXITED
    ERR BUSY | RANGE | STATE | UNKNOWN
*/

#include <LiquidCrystal.h>
#include <EEPROM.h>

//=================================== HARDWARE ===================================
const int rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);

const int buzzerpin    = 7;
const int rightButton  = 8;   // previous / decrease
const int leftButton   = 9;   // next / increase
const int selectButton = 10;  // select; pause while a timer is running
const int muteButton   = 6;   // mute / unmute (button between pin 6 and GND)

//music tone setup for "SO" (G), "LA" (A), "TI" (B), "DO" (C)
int abc[] = {3920, 4400, 4940, 5230};

//=================================== SETTINGS ===================================
// Ranges offered by the on-device CUSTOM menu
const int TIMER_MIN  = 25, TIMER_MAX  = 60, TIMER_STEP = 5;
const int BREAK_MIN  = 5,  BREAK_MAX  = 30, BREAK_STEP = 5;
const int CYCLES_MIN = 1,  CYCLES_MAX = 6;

// Limits for values sent from the computer (LCD shows 2-digit minutes)
const int REMOTE_MAX_MINS = 99, REMOTE_MAX_CYCLES = 20;

const int AUTO_FOCUS = 25, AUTO_BREAK = 5;
const unsigned long GET_READY_MS = 6000;   // shows 5, 4, 3, 2, 1, 0

int focusMins = 25, breakMins = 5, cycles = 1;   // last choice in the CUSTOM menu

// Mute setting is saved in EEPROM so it survives a restart (connecting over USB restarts the Uno)
const int MUTE_ADDR = 0;
bool muted = false;

//=================================== STATES =====================================
enum {
  MAIN_MENU, PICK_TIMER, PICK_BREAK, PICK_CYCLES,             // menus
  GET_READY, FOCUS, REST, PAUSED,                             // timers
  FOCUS_DONE, CHECKPOINT, CONTINUE_PROMPT,                    // between timers
  SESSION_ENDED, TIMER_EXITED, SUMMARY                        // end of a session
};

int state = MAIN_MENU;
int pausedFrom = FOCUS;     // which timer was paused (FOCUS or REST)
int menuSel = 0;            // highlighted option on two-choice screens
int pickVal = 25;           // value shown on the TIMER / BREAK / CYCLES screens

// Current session
bool autoMode = false;
int sessFocus = 25, sessBreak = 5, sessCycles = 0;   // sessCycles = 0 in auto mode
int cycleNum = 0;
int count = 0;          // focus periods completed
int totalFocus = 0;     // minutes focused

// Timing (all from millis(), so there's no drift from delays)
unsigned long phaseEnd = 0;     // when the current timer hits zero
unsigned long pausedLeft = 0;   // ms that were left when paused
unsigned long screenEnd = 0;    // when a message screen should move on

// Display / USB bookkeeping
bool redraw = true;
long lastTimeKey = -1;
unsigned long lastStatus = 0;
long lastSentSecs = -1;
char line[17];
char rx[40];
byte rxLen = 0;

//=================================== SETUP / LOOP ===============================
void setup(){
  Serial.begin(115200);
  lcd.begin(16,2);
  pinMode(rightButton, INPUT_PULLUP);
  pinMode(leftButton, INPUT_PULLUP);
  pinMode(selectButton, INPUT_PULLUP);
  pinMode(muteButton, INPUT_PULLUP);
  pinMode(buzzerpin, OUTPUT);
  muted = (EEPROM.read(MUTE_ADDR) == 1);   // a blank EEPROM reads 255 = not muted

  lcd.setCursor(3,0);
  lcd.print(F("WELCOME TO"));
  lcd.setCursor(4,1);
  lcd.print(F("POMODORO"));
  for (int i = 0; i < 4; i++){   //playing welcoming melody
    mtone(buzzerpin, abc[i], 500);
    delay(50);
  }

  Serial.println(F("READY"));
  goTo(MAIN_MENU);
}

void loop(){
  readSerial();
  int btn = readButtons();
  if (btn >= 0) handleButton(btn);
  update();
  draw();

  // Tell the computer whenever the seconds change, plus a heartbeat every second
  if (statusSecs() != lastSentSecs || millis() - lastStatus >= 1000) sendStatus();
}

//=================================== STATE CHANGES ==============================
void goTo(int s){
  state = s;
  redraw = true;
  draw();
  sendStatus();
}

// Show a message screen for 'ms' milliseconds (update() moves on afterwards)
void showFor(int s, unsigned long ms){
  screenEnd = millis() + ms;
  goTo(s);
}

bool screenOver(){
  return (long)(millis() - screenEnd) >= 0;
}

bool inSession(){
  return state >= GET_READY && state <= CONTINUE_PROMPT;
}

bool isTimed(){
  return state == GET_READY || state == FOCUS || state == REST || state == PAUSED;
}

unsigned long msLeft(){
  if (state == PAUSED) return pausedLeft;
  long d = (long)(phaseEnd - millis());
  return d > 0 ? d : 0;
}

// Seconds left as shown to the user (GET READY counts 5..0, timers round up)
long statusSecs(){
  if (!isTimed()) return -1;
  if (state == GET_READY) return msLeft() / 1000;
  return (msLeft() + 999) / 1000;
}

//=================================== SESSIONS ===================================
void startSession(bool isAuto, int f, int b, int c){
  autoMode = isAuto;
  sessFocus = f;
  sessBreak = b;
  sessCycles = isAuto ? 0 : c;
  cycleNum = 1;
  count = 0;
  totalFocus = 0;
  startGetReady();
}

void startGetReady(){
  phaseEnd = millis() + GET_READY_MS;
  goTo(GET_READY);
  buzz();
}

void startPhase(int s, int mins){
  phaseEnd = millis() + (unsigned long)mins * 60000UL;
  goTo(s);
}

void focusFinished(){
  count++;
  totalFocus += sessFocus;
  Serial.println(F("EVENT FOCUS_DONE"));

  if (!autoMode && cycleNum >= sessCycles){   // last cycle: skip the break
    endSession();
    return;
  }
  showFor(FOCUS_DONE, 2000);
  buzz();
}

void restFinished(){
  Serial.println(F("EVENT REST_DONE"));
  showFor(CHECKPOINT, 1500);
  click(100);
}

void endSession(){
  Serial.println(F("EVENT SESSION_ENDED"));
  showFor(SESSION_ENDED, 2000);
  for (int i = 0; i < 3; i++){
    click(100);
    delay(100);
  }
}

void exitSession(){
  Serial.println(F("EVENT EXITED"));
  showFor(TIMER_EXITED, 1500);
  buzz();
}

void pauseTimer(){
  pausedLeft = msLeft();   // still in FOCUS/REST here
  pausedFrom = state;
  menuSel = 0;
  goTo(PAUSED);
}

void resumeTimer(){
  phaseEnd = millis() + pausedLeft;
  goTo(pausedFrom);
}

void answerContinue(bool yes){
  if (yes){
    cycleNum++;
    startGetReady();
  }
  else showFor(SUMMARY, 2500);
}

// Moves between states when a timer or message screen runs out
void update(){
  switch (state){
    case GET_READY:
      if (msLeft() == 0) startPhase(FOCUS, sessFocus);
      break;

    case FOCUS:
      if (msLeft() == 0) focusFinished();
      break;

    case REST:
      if (msLeft() == 0) restFinished();
      break;

    case FOCUS_DONE:
      if (screenOver()) startPhase(REST, sessBreak);
      break;

    case CHECKPOINT:
      if (screenOver()){
        if (autoMode){
          menuSel = 0;
          goTo(CONTINUE_PROMPT);
        }
        else {
          cycleNum++;
          startGetReady();
        }
      }
      break;

    case SESSION_ENDED:
    case TIMER_EXITED:
      if (screenOver()) showFor(SUMMARY, 2500);
      break;

    case SUMMARY:
      if (screenOver()){
        menuSel = 0;
        goTo(MAIN_MENU);
      }
      break;
  }
}

//=================================== BUTTONS ====================================
const int NUM_BUTTONS = 4;
const int btnPins[NUM_BUTTONS] = {leftButton, rightButton, selectButton, muteButton};
bool btnDown[NUM_BUTTONS] = {false, false, false, false};
unsigned long btnChanged[NUM_BUTTONS] = {0, 0, 0, 0};

// Returns the pin of a button that was just pressed, or -1. Never waits.
int readButtons(){
  for (int i = 0; i < NUM_BUTTONS; i++){
    bool down = !digitalRead(btnPins[i]);
    if (down != btnDown[i] && millis() - btnChanged[i] > 30){   // 30 ms debounce
      btnDown[i] = down;
      btnChanged[i] = millis();
      if (down) return btnPins[i];
    }
  }
  return -1;
}

void pickerRange(int s, int &mn, int &mx, int &st){
  if (s == PICK_TIMER)      { mn = TIMER_MIN;  mx = TIMER_MAX;  st = TIMER_STEP; }
  else if (s == PICK_BREAK) { mn = BREAK_MIN;  mx = BREAK_MAX;  st = BREAK_STEP; }
  else                      { mn = CYCLES_MIN; mx = CYCLES_MAX; st = 1; }
}

void handleButton(int btn){
  if (btn == muteButton){        // works on every screen
    setMuted(!muted);
    return;
  }
  mtone(buzzerpin, btn == selectButton ? abc[3] : abc[0], 50);
  bool next = (btn == leftButton);
  bool prev = (btn == rightButton);
  bool sel  = (btn == selectButton);

  switch (state){
    case MAIN_MENU:
      if (next) menuSel = 1;
      else if (prev) menuSel = 0;
      else if (menuSel == 0) { startSession(true, AUTO_FOCUS, AUTO_BREAK, 0); return; }
      else { pickVal = focusMins; goTo(PICK_TIMER); return; }
      break;

    case PICK_TIMER:
    case PICK_BREAK:
    case PICK_CYCLES: {
      int mn, mx, st;
      pickerRange(state, mn, mx, st);
      int back = mn - st;                     // one step below the minimum = BACK
      if (next && pickVal < mx) pickVal += st;
      else if (prev && pickVal > back) pickVal -= st;
      else if (sel) { pickerSelect(pickVal == back); return; }
      break;
    }

    case FOCUS:
    case REST:
      if (sel) pauseTimer();
      return;

    case PAUSED:
      if (next) menuSel = 1;
      else if (prev) menuSel = 0;
      else if (menuSel == 0) { resumeTimer(); return; }
      else { exitSession(); return; }
      break;

    case CONTINUE_PROMPT:
      if (next) menuSel = 1;
      else if (prev) menuSel = 0;
      else { answerContinue(menuSel == 0); return; }
      break;

    default:
      return;   // buttons do nothing on message screens
  }
  redraw = true;
}

// CUSTOM -> TIMER -> BREAK -> CYCLES. BACK goes to the previous screen.
void pickerSelect(bool back){
  if (state == PICK_TIMER){
    if (back) { menuSel = 1; goTo(MAIN_MENU); }
    else { focusMins = pickVal; pickVal = breakMins; goTo(PICK_BREAK); }
  }
  else if (state == PICK_BREAK){
    if (back) { pickVal = focusMins; goTo(PICK_TIMER); }
    else { breakMins = pickVal; pickVal = cycles; goTo(PICK_CYCLES); }
  }
  else {
    if (back) { pickVal = breakMins; goTo(PICK_BREAK); }
    else { cycles = pickVal; startSession(false, focusMins, breakMins, cycles); }
  }
}

//=================================== USB SERIAL =================================
void readSerial(){
  while (Serial.available()){
    char c = Serial.read();
    if (c == '\n' || c == '\r'){
      if (rxLen > 0){
        rx[rxLen] = '\0';
        handleCommand(rx);
        rxLen = 0;
      }
    }
    else if (rxLen < sizeof(rx) - 1){
      rx[rxLen++] = toupper(c);
    }
  }
}

void handleCommand(char *cmd){
  int f, b, c;

  if (strcmp(cmd, "STATUS") == 0){
    sendStatus();
  }
  else if (strcmp(cmd, "AUTO") == 0){
    if (inSession()) Serial.println(F("ERR BUSY"));
    else startSession(true, AUTO_FOCUS, AUTO_BREAK, 0);
  }
  else if (strncmp(cmd, "START", 5) == 0){
    if (sscanf(cmd, "START %d %d %d", &f, &b, &c) != 3) Serial.println(F("ERR UNKNOWN"));
    else if (inSession()) Serial.println(F("ERR BUSY"));
    else if (f < 1 || f > REMOTE_MAX_MINS || b < 1 || b > REMOTE_MAX_MINS ||
             c < 1 || c > REMOTE_MAX_CYCLES) Serial.println(F("ERR RANGE"));
    else startSession(false, f, b, c);
  }
  else if (strcmp(cmd, "PAUSE") == 0){
    if (state == FOCUS || state == REST) pauseTimer();
    else Serial.println(F("ERR STATE"));
  }
  else if (strcmp(cmd, "RESUME") == 0){
    if (state == PAUSED) resumeTimer();
    else Serial.println(F("ERR STATE"));
  }
  else if (strcmp(cmd, "EXIT") == 0){
    if (inSession()) exitSession();
    else Serial.println(F("ERR STATE"));
  }
  else if (strcmp(cmd, "MUTE") == 0){
    setMuted(true);
  }
  else if (strcmp(cmd, "UNMUTE") == 0){
    setMuted(false);
  }
  else if (strcmp(cmd, "YES") == 0 || strcmp(cmd, "NO") == 0){
    if (state == CONTINUE_PROMPT) answerContinue(cmd[0] == 'Y');
    else Serial.println(F("ERR STATE"));
  }
  else {
    Serial.println(F("ERR UNKNOWN"));
  }
}

const char* stateName(){
  switch (state){
    case GET_READY:       return "GETREADY";
    case FOCUS:           return "FOCUS";
    case REST:            return "REST";
    case PAUSED:          return pausedFrom == FOCUS ? "PAUSED_FOCUS" : "PAUSED_REST";
    case FOCUS_DONE:      return "FOCUS_DONE";
    case CHECKPOINT:      return "CHECKPOINT";
    case CONTINUE_PROMPT: return "CONTINUE";
    case SESSION_ENDED:   return "ENDED";
    case TIMER_EXITED:    return "EXITED";
    case SUMMARY:         return "SUMMARY";
    default:              return "MENU";
  }
}

void sendStatus(){
  long secs = statusSecs();
  lastStatus = millis();
  lastSentSecs = secs;

  Serial.print(F("STATE state="));  Serial.print(stateName());
  Serial.print(F(" left="));        Serial.print(secs < 0 ? 0 : secs);
  Serial.print(F(" cycle="));       Serial.print(cycleNum);
  Serial.print(F(" cycles="));      Serial.print(sessCycles);
  Serial.print(F(" focus="));       Serial.print(sessFocus);
  Serial.print(F(" break="));       Serial.print(sessBreak);
  Serial.print(F(" done="));        Serial.print(count);
  Serial.print(F(" total="));       Serial.print(totalFocus);
  Serial.print(F(" mode="));        Serial.print(autoMode ? F("AUTO") : F("CUSTOM"));
  Serial.print(F(" muted="));       Serial.println(muted ? 1 : 0);
}

//=================================== LCD ========================================
void draw(){
  if (redraw){
    redraw = false;
    lastTimeKey = -1;
    lcd.clear();

    switch (state){
      case MAIN_MENU:    drawChoice("---- OPTION ----", "AUTO", "CUSTOM"); break;
      case PICK_TIMER:   drawPicker("----- TIMER ----", "MIN", "MIN"); break;
      case PICK_BREAK:   drawPicker("----- BREAK ----", "MIN", "MIN"); break;
      case PICK_CYCLES:  drawPicker("---- CYCLES ----", "CYCLE", "CYCLES"); break;
      case GET_READY:    lcd.print(F("---- ALERT! ----")); break;
      case FOCUS:        lcd.print(F("--- FOCUSING ---")); break;
      case REST:         lcd.print(F("----- REST -----")); break;

      case PAUSED: {
        char title[17];
        long secs = (pausedLeft + 999) / 1000;
        snprintf(title, 17, "PAUSED     %02d:%02d", (int)(secs / 60), (int)(secs % 60));
        drawChoice(title, "RESUME", "EXIT");
        break;
      }

      case FOCUS_DONE:
        lcd.print(F("-- COMPLETED! --"));
        lcd.setCursor(3,1);
        lcd.print(F("REST TIME"));
        break;

      case CHECKPOINT:
        lcd.print(F("-- CHECKPOINT --"));
        lcd.setCursor(0,1);
        printCount();
        break;

      case CONTINUE_PROMPT: drawChoice("-- CONTINUE ? --", "YES", "NO"); break;

      case SESSION_ENDED:
        lcd.print(F("SESSION"));
        lcd.setCursor(0,1);
        lcd.print(F("ENDED"));
        break;

      case TIMER_EXITED: lcd.print(F("- TIMER EXITED -")); break;

      case SUMMARY:
        lcd.print(F("--- SUMMARY ----"));
        lcd.setCursor(0,1);
        printCount();
        break;
    }
  }

  // Bottom row of the timer screens only changes when the number (or colon) changes
  if (state == GET_READY){
    long n = msLeft() / 1000;
    if (n != lastTimeKey){
      lastTimeKey = n;
      lcd.setCursor(1,1);
      lcd.print(F("GET READY IN "));
      lcd.print(n);
    }
  }
  else if (state == FOCUS || state == REST){
    unsigned long left = msLeft();
    long secs = (left + 999) / 1000;
    bool colon = (left % 1000) < 700;     // colon blinks like the original
    long key = secs * 2 + colon;
    if (key != lastTimeKey){
      lastTimeKey = key;
      showTime(secs, colon);
    }
  }
}

// Two-option screen (AUTO/CUSTOM, RESUME/EXIT, YES/NO)
void drawChoice(const char* title, const char* a, const char* b){
  lcd.print(title);
  lcd.setCursor(0,1);
  snprintf(line, 17, " %c%-6s %c%-6s", menuSel == 0 ? '~' : ' ', a, menuSel == 1 ? '~' : ' ', b);
  lcd.print(line);
}

// Number picker: "< 30 MIN >", or "< BACK >" one step below the minimum
void drawPicker(const char* title, const char* unitOne, const char* unitMany){
  int mn, mx, st;
  pickerRange(state, mn, mx, st);
  if (pickVal < mn) snprintf(line, 17, "< BACK >");
  else snprintf(line, 17, "< %d %s >", pickVal, pickVal == 1 ? unitOne : unitMany);

  lcd.print(title);
  lcd.setCursor((16 - strlen(line)) / 2, 1);
  lcd.print(line);
}

// MM :: SS on the bottom row
void showTime(long secs, bool colon){
  int m = secs / 60;
  int s = secs % 60;
  if (colon) snprintf(line, 17, "    %02d :: %02d    ", m, s);
  else       snprintf(line, 17, "    %02d    %02d    ", m, s);
  lcd.setCursor(0,1);
  lcd.print(line);
}

// COUNT = focus sessions finished, then total focus time as HH:MM
void printCount(){
  snprintf(line, 17, "COUNT:%02d   %02d:%02d", count, totalFocus / 60, totalFocus % 60);
  lcd.print(line);
}

//=================================== SOUND ======================================
// Every sound goes through click() or mtone(), so muting silences everything.

void setMuted(bool m){
  muted = m;
  EEPROM.update(MUTE_ADDR, m ? 1 : 0);   // only writes if the value changed

  // Show it on the top row for a moment, then put the normal screen back
  lcd.setCursor(0,0);
  lcd.print(m ? F("-- SOUND OFF  --") : F("--- SOUND ON ---"));
  if (!m) mtone(buzzerpin, abc[3], 80);   // confirmation beep when unmuting
  delay(700);
  redraw = true;
  draw();
  sendStatus();
}

void click(int ms){
  if (muted) { delay(ms); return; }
  digitalWrite(buzzerpin, HIGH);
  delay(ms);
  digitalWrite(buzzerpin, LOW);
}

void buzz(){
  for (int i = 0; i < 3; i++){
    click(50);
    delay(50);
  }
}

void mtone(int dx, int hz, unsigned long tm){
  if (muted) { delay(tm); return; }   // same timing, no sound
  unsigned long t = millis();
  unsigned long ns = (long)500000 / hz;
  while (millis() - t < tm){
    digitalWrite(dx, HIGH);
    delayMicroseconds(ns);
    digitalWrite(dx, LOW);
    delayMicroseconds(ns);
  }
}