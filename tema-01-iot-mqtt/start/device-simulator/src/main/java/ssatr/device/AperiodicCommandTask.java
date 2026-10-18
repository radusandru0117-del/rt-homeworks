package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK APERIODIC: execută comenzile primite de la business service.
 *
 * Comenzile sosesc oricând, pe MQTT, fără un interval minim între ele, și sunt
 * puse în coada commands. Fiecare comandă eliberează un job al taskului.
 * Comenzi care trebuie suportate:
 *
 *   SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
 *   SET_THRESHOLD <valoare> schimbă pragul de alarmă
 *   RESET                   readuce senzorul la valoarea inițială
 *   STATUS                  raportează perioada și pragul curente
 *
 * După executarea fiecărei comenzi, device-ul publică pe Config.statusTopic(deviceId)
 * o confirmare: "deviceId;timestamp;text", de exemplu
 * "device-1;1727600000000;OK SET_INTERVAL 1000".
 */
public class AperiodicCommandTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<String> commands;
    private final PeriodicTelemetryTask periodic;

    public AperiodicCommandTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                BlockingQueue<String> commands, PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.commands = commands;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String command = commands.take().trim();   // fiecare comandă eliberează un job

                String reply;
                try {
                    reply = execute(command);
                } catch (Exception e) {
                    // parametru invalid (de ex. "SET_INTERVAL abc"): taskul nu se oprește
                    reply = "ERROR parametru invalid: " + command;
                }

                mqtt.publish(Config.statusTopic(deviceId),
                        deviceId + ";" + System.currentTimeMillis() + ";" + reply);
                System.out.println("[aperiodic] " + command + " -> " + reply);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** Execută o comandă și întoarce confirmarea ("OK ..." sau "ERROR ..."). */
    private String execute(String command) {
        String[] parts = command.split(" ");

        switch (parts[0]) {
            case "SET_INTERVAL":
                long ms = Long.parseLong(parts[1]);
                if (ms <= 0) {
                    return "ERROR intervalul trebuie sa fie pozitiv: " + ms;
                }
                periodic.setPeriodMs(ms);
                return "OK SET_INTERVAL " + ms;
            case "SET_THRESHOLD":
                periodic.setThreshold(Double.parseDouble(parts[1]));
                return "OK SET_THRESHOLD " + parts[1];
            case "RESET":
                sensor.reset();
                return "OK RESET";
            case "STATUS":
                return String.format(Locale.US, "OK STATUS interval=%d prag=%.2f",
                        periodic.getPeriodMs(), periodic.getThreshold());
            default:
                return "ERROR comanda necunoscuta: " + parts[0];
        }
    }
}
