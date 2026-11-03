// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// ---------------------------------------------------------------------------
// Starea comună și cozile
// ---------------------------------------------------------------------------

// Perioada și pragul: scrise de taskul aperiodic, citite de taskul periodic.
volatile uint32_t period_ms = PERIOD_MS_INITIAL;
volatile float threshold = THRESHOLD_INITIAL;

// Evenimentul de alarmă, pus în coadă prin valoare.
struct AlarmEvent {
    float temperature;
    float threshold;
    uint32_t arrival_ms;   // momentul sosirii (millis()), pentru intervalul minim
};

// Comanda primită, pusă în coadă prin valoare: textul însuși, nu un pointer.
struct Command {
    char text[COMMAND_MAX_LEN];
};

const UBaseType_t ALARM_QUEUE_LEN = 10;
const UBaseType_t COMMAND_QUEUE_LEN = 10;

QueueHandle_t alarm_queue = NULL;
QueueHandle_t command_queue = NULL;

// Prioritățile: periodicul are termenul cel mai strict, apoi alarmele;
// comenzile sunt cele mai puțin urgente.
const UBaseType_t PRIO_PERIODIC = 4;
const UBaseType_t PRIO_SPORADIC = 3;
const UBaseType_t PRIO_APERIODIC = 2;
const uint32_t TASK_STACK = 4096;

// ---------------------------------------------------------------------------
// TODO 1: TASK PERIODIC
// ---------------------------------------------------------------------------
void periodic_task(void* arg) {
    TickType_t last = xTaskGetTickCount();   // r_0
    for (;;) {
        float t = read_temperature();
        float thr = threshold;               // o singură citire pe job

        char msg[96];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f", DEVICE_ID, (long long)epoch_ms(), t);
        mqtt_publish("telemetry", msg);
        Serial.printf("[periodic] T=%lu ms, %s\n", (unsigned long)period_ms, msg);

        if (t > thr) {
            AlarmEvent ev = { t, thr, millis() };
            if (xQueueSend(alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme este plină, eveniment pierdut");
            }
        }

        // așteptare absolută: următoarea eliberare este la last + T
        xTaskDelayUntil(&last, pdMS_TO_TICKS(period_ms));
    }
}

// ---------------------------------------------------------------------------
// TODO 2: TASK SPORADIC
// ---------------------------------------------------------------------------
void sporadic_task(void* arg) {
    bool first = true;
    uint32_t last_accepted_ms = 0;
    for (;;) {
        AlarmEvent ev;
        xQueueReceive(alarm_queue, &ev, portMAX_DELAY);

        uint32_t since = ev.arrival_ms - last_accepted_ms;
        if (!first && since < MIN_INTERARRIVAL_MS) {
            Serial.printf("[sporadic] alarmă respinsă: %.2f > %.2f, la %lu ms după ultima (minim %lu)\n",
                          ev.temperature, ev.threshold,
                          (unsigned long)since, (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        first = false;
        last_accepted_ms = ev.arrival_ms;

        char msg[112];
        snprintf(msg, sizeof(msg), "%s;%lld;%.2f;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), ev.temperature, ev.threshold);
        mqtt_publish("alarm", msg);
        Serial.printf("[sporadic] ALARMĂ publicată: %s\n", msg);
    }
}

// ---------------------------------------------------------------------------
// TODO 3: TASK APERIODIC
// ---------------------------------------------------------------------------

// Execută o comandă și scrie în reply confirmarea ("OK ..." sau "ERROR ...").
void execute_command(char* command, char* reply, size_t size) {
    char* save = NULL;
    char* name = strtok_r(command, " \t\r\n", &save);
    char* param = strtok_r(NULL, " \t\r\n", &save);

    if (name == NULL) {
        snprintf(reply, size, "ERROR comanda goala");
    } else if (strcmp(name, "SET_INTERVAL") == 0) {
        char* end = NULL;
        long ms = param ? strtol(param, &end, 10) : 0;
        if (param == NULL || *end != '\0') {
            snprintf(reply, size, "ERROR parametru invalid: %s %s", name, param ? param : "");
        } else if (ms <= 0) {
            snprintf(reply, size, "ERROR intervalul trebuie sa fie pozitiv: %ld", ms);
        } else {
            period_ms = (uint32_t)ms;
            snprintf(reply, size, "OK SET_INTERVAL %ld", ms);
        }
    } else if (strcmp(name, "SET_THRESHOLD") == 0) {
        char* end = NULL;
        float value = param ? strtof(param, &end) : 0;
        if (param == NULL || *end != '\0') {
            snprintf(reply, size, "ERROR parametru invalid: %s %s", name, param ? param : "");
        } else {
            threshold = value;
            snprintf(reply, size, "OK SET_THRESHOLD %s", param);
        }
    } else if (strcmp(name, "RESET") == 0) {
        period_ms = PERIOD_MS_INITIAL;
        threshold = THRESHOLD_INITIAL;
        snprintf(reply, size, "OK RESET");
    } else if (strcmp(name, "STATUS") == 0) {
        snprintf(reply, size, "OK STATUS interval=%lu prag=%.2f",
                 (unsigned long)period_ms, threshold);
    } else {
        snprintf(reply, size, "ERROR comanda necunoscuta: %s", name);
    }
}

void aperiodic_task(void* arg) {
    for (;;) {
        Command cmd;
        xQueueReceive(command_queue, &cmd, portMAX_DELAY);   // fiecare comandă eliberează un job

        char original[COMMAND_MAX_LEN];
        strlcpy(original, cmd.text, sizeof(original));        // strtok_r modifică textul

        char reply[96];
        execute_command(cmd.text, reply, sizeof(reply));

        char msg[160];
        snprintf(msg, sizeof(msg), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), reply);
        mqtt_publish("status", msg);
        Serial.printf("[aperiodic] %s -> %s\n", original, reply);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    Command cmd;
    strlcpy(cmd.text, text, sizeof(cmd.text));
    if (command_queue == NULL || xQueueSend(command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi este plină, comandă pierdută");
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
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, înainte de mqtt_start()
    alarm_queue = xQueueCreate(ALARM_QUEUE_LEN, sizeof(AlarmEvent));
    command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command));
    if (alarm_queue == NULL || command_queue == NULL) {
        Serial.println("EROARE: cozile nu au putut fi create");
        for (;;) delay(1000);
    }

    mqtt_start();

    // TODO 0 (b): taskurile, după mqtt_start()
    xTaskCreate(periodic_task, "periodic", TASK_STACK, NULL, PRIO_PERIODIC, NULL);
    xTaskCreate(sporadic_task, "sporadic", TASK_STACK, NULL, PRIO_SPORADIC, NULL);
    xTaskCreate(aperiodic_task, "aperiodic", TASK_STACK, NULL, PRIO_APERIODIC, NULL);
    Serial.printf("taskuri pornite: periodic (prio %u), sporadic (prio %u), aperiodic (prio %u)\n",
                  (unsigned)PRIO_PERIODIC, (unsigned)PRIO_SPORADIC, (unsigned)PRIO_APERIODIC);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
