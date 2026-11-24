// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// ---------------------------------------------------------------------------
// TODO 0 (c): starea semaforului, protejată de mutex
// ---------------------------------------------------------------------------

struct TrafficState {
    Phase phase;               // faza curentă
    uint32_t phase_start_ms;   // momentul intrării în faza curentă (millis())
    bool ped_request;          // cererea pietonului = lampa AȘTEPTAȚI
};

TrafficState state = { CAR_GREEN, 0, false };
SemaphoreHandle_t state_mutex = NULL;

// O apăsare a butonului, pusă în coadă prin valoare. Momentul se ia de cel
// care o trimite (ISR-ul sau command_received()), nu de taskul care o preia.
enum PressSource { SRC_BOOT, SRC_MQTT };
struct ButtonPress {
    uint32_t time_ms;          // momentul apăsării, din tick-uri (timp monoton)
    PressSource source;
};

const UBaseType_t BUTTON_QUEUE_LEN = 16;   // încap și întreruperile de la bounce
QueueHandle_t button_queue = NULL;

// Prioritățile. Cele două taskuri periodice după Rate Monotonic: perioada mai
// mică, prioritatea mai mare. Taskul sporadic, cu intervalul minim între sosiri
// de 1000 ms, are după aceeași regulă prioritatea cea mai mică.
const UBaseType_t PRIO_CONTROL = 5;    // T = 100 ms
const UBaseType_t PRIO_OUTPUT = 4;     // T = 500 ms
const UBaseType_t PRIO_BUTTON = 3;     // sporadic, T_min = 1000 ms
const uint32_t TASK_STACK = 4096;

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC DE CONTROL
// ---------------------------------------------------------------------------
void control_task(void* arg) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        bool changed = false;
        Phase new_phase;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        uint32_t now = millis();
        uint32_t elapsed = now - state.phase_start_ms;
        new_phase = state.phase;
        switch (state.phase) {
            case CAR_GREEN:
                if (elapsed >= CAR_GREEN_MIN_MS && state.ped_request) new_phase = CAR_YELLOW;
                break;
            case CAR_YELLOW:
                if (elapsed >= CAR_YELLOW_MS) new_phase = ALL_RED;
                break;
            case ALL_RED:
                if (elapsed >= ALL_RED_MS) new_phase = PED_GREEN;
                break;
            case PED_GREEN:
                if (elapsed >= PED_GREEN_MS) new_phase = CAR_GREEN;
                break;
        }
        if (new_phase != state.phase) {
            state.phase = new_phase;
            state.phase_start_ms = now;
            if (new_phase == PED_GREEN) state.ped_request = false;   // pietonul primește ce a cerut
            changed = true;
        }
        xSemaphoreGive(state_mutex);

        if (changed) {
            Serial.printf("[control] %lu ms: faza %s\n", (unsigned long)millis(), PHASE_NAMES[new_phase]);
        }

        xTaskDelayUntil(&last, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK PERIODIC DE IEȘIRE
// ---------------------------------------------------------------------------
void output_task(void* arg) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        // copia stării, cu mutexul ținut cât mai puțin
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        TrafficState s = state;
        uint32_t now = millis();
        xSemaphoreGive(state_mutex);

        Light cars = RED;
        Light peds = RED;
        switch (s.phase) {
            case CAR_GREEN:  cars = GREEN;  peds = RED;   break;
            case CAR_YELLOW: cars = YELLOW; peds = RED;   break;
            case ALL_RED:    cars = RED;    peds = RED;   break;
            case PED_GREEN:  cars = RED;    peds = GREEN; break;
        }

        // secundele rămase pentru pietoni, rotunjite în sus; 0 = nu se afișează
        uint32_t seconds = 0;
        if (s.phase == PED_GREEN) {
            uint32_t elapsed = now - s.phase_start_ms;
            uint32_t remaining = elapsed < PED_GREEN_MS ? PED_GREEN_MS - elapsed : 0;
            seconds = (remaining + 999) / 1000;
            if (seconds == 0) seconds = 1;   // faza n-a fost încă închisă de taskul de control
        }

        char msg[112];
        snprintf(msg, sizeof(msg), "%s;%lld;%s;%s;%s;%d;%lu",
                 DEVICE_ID, (long long)epoch_ms(), PHASE_NAMES[s.phase],
                 LIGHT_NAMES[cars], LIGHT_NAMES[peds],
                 s.ped_request ? 1 : 0, (unsigned long)seconds);
        mqtt_publish("lights", msg);
        digitalWrite(LED_PIN, s.ped_request ? LOW : HIGH);   // LED-ul e aprins pe LOW

        xTaskDelayUntil(&last, pdMS_TO_TICKS(OUTPUT_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// TODO 3: TASK SPORADIC, apăsările butonului pentru pietoni
// ---------------------------------------------------------------------------
void button_task(void* arg) {
    bool first = true;
    uint32_t last_accepted_ms = 0;
    for (;;) {
        ButtonPress press;
        xQueueReceive(button_queue, &press, portMAX_DELAY);
        const char* src = press.source == SRC_BOOT ? "BOOT" : "BUTTON (MQTT)";

        uint32_t since = press.time_ms - last_accepted_ms;
        if (!first && since < MIN_INTERARRIVAL_MS) {
            Serial.printf("[buton] apăsare %s respinsă: la %lu ms după ultima acceptată (minim %lu)\n",
                          src, (unsigned long)since, (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        first = false;
        last_accepted_ms = press.time_ms;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        bool already = state.ped_request;
        state.ped_request = true;
        xSemaphoreGive(state_mutex);

        Serial.printf("[buton] apăsare %s acceptată%s\n", src,
                      already ? " (cererea era deja înregistrată)" : ": cerere înregistrată, AȘTEPTAȚI aprinsă");
    }
}

// ---------------------------------------------------------------------------
// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT
// ---------------------------------------------------------------------------
void IRAM_ATTR button_isr(void) {
    ButtonPress press = { (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS), SRC_BOOT };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);   // coada plină: apăsarea se pierde
    portYIELD_FROM_ISR(woken);   // dacă a fost trezit un task mai prioritar, rulează imediat
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);

    // comparăm fără spațiile sau sfârșitul de linie de la final
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1])) n--;
    if (n != 6 || strncmp(text, "BUTTON", 6) != 0) return;   // alte comenzi se ignoră

    ButtonPress press = { (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS), SRC_MQTT };
    if (button_queue == NULL || xQueueSend(button_queue, &press, 0) != pdTRUE) {
        Serial.println("[mqtt] coada butonului este plină, apăsare pierdută");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): mutexul stării și coada butonului, înainte de mqtt_start()
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(BUTTON_QUEUE_LEN, sizeof(ButtonPress));
    if (state_mutex == NULL || button_queue == NULL) {
        Serial.println("EROARE: mutexul sau coada nu au putut fi create");
        for (;;) delay(1000);
    }
    state.phase = CAR_GREEN;
    state.phase_start_ms = millis();   // intrarea în faza inițială
    state.ped_request = false;

    mqtt_start();

    // TODO 0 (b): taskurile, apoi întreruperea butonului
    xTaskCreate(control_task, "control", TASK_STACK, NULL, PRIO_CONTROL, NULL);
    xTaskCreate(output_task, "output", TASK_STACK, NULL, PRIO_OUTPUT, NULL);
    xTaskCreate(button_task, "button", TASK_STACK, NULL, PRIO_BUTTON, NULL);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);

    Serial.printf("taskuri pornite: control (prio %u), ieșire (prio %u), buton (prio %u); faza %s\n",
                  (unsigned)PRIO_CONTROL, (unsigned)PRIO_OUTPUT, (unsigned)PRIO_BUTTON,
                  PHASE_NAMES[CAR_GREEN]);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
