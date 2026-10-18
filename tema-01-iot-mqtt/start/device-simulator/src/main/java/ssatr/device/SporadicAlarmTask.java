package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;

/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    private long lastRelease = 0;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        try {
            while (true) {
                AlarmEvent e = alarmEvents.take();   // așteaptă (blocat) următorul eveniment
                long now = System.currentTimeMillis();

                long sinceLast = now - lastRelease;
                if (sinceLast < MIN_INTERARRIVAL_MS) {
                    System.out.printf("[sporadic]  alarmă RESPINSĂ: au trecut doar %d ms (interval minim %d ms)%n",
                            sinceLast, MIN_INTERARRIVAL_MS);
                    continue;
                }
                lastRelease = now;

                String msg = deviceId + ";" + now + ";"
                        + String.format(Locale.US, "%.2f;%.2f", e.temperature(), e.threshold());
                mqtt.publish(Config.alarmTopic(deviceId), msg);
                System.out.println("[sporadic]  ALARMĂ publicată: " + msg);
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
        }
    }
}
