package ssatr.device;

import java.util.Locale;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<AlarmEvent> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<AlarmEvent> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        // r_0: prima eliberare este chiar acum (timp monoton)
        long next = System.nanoTime();

        try {
            while (true) {
                // ---- jobul ----
                double t = sensor.read();
                String msg = deviceId + ";" + System.currentTimeMillis() + ";"
                        + String.format(Locale.US, "%.2f", t);
                mqtt.publish(Config.telemetryTopic(deviceId), msg);
                System.out.println("[periodic]  " + msg);

                if (t > threshold) {
                    alarmEvents.put(new AlarmEvent(t, threshold));
                }

                // ---- așteptare absolută până la următoarea eliberare de pe grilă ----
                // next = next + T, nu "acum + T", deci durata jobului nu se adună (fără derivă)
                next += TimeUnit.MILLISECONDS.toNanos(periodMs);
                long remaining = next - System.nanoTime();
                if (remaining > 0) {
                    TimeUnit.NANOSECONDS.sleep(remaining);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
