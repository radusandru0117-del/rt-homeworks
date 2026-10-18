package ssatr.service;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme etc.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci accesul trebuie sincronizat: toate metodele sunt synchronized.
 */
public class Dashboard {

    /** Starea unui device. */
    private static class DeviceState {
        double lastTemperature = Double.NaN;   // NaN până la prima telemetrie
        long lastSeen;                         // momentul ultimului mesaj primit
        int alarms;
    }

    private final Map<String, DeviceState> devices = new HashMap<>();

    /** Întoarce starea device-ului (o creează la primul mesaj) și notează momentul mesajului. */
    private DeviceState seen(String deviceId) {
        DeviceState s = devices.computeIfAbsent(deviceId, id -> new DeviceState());
        s.lastSeen = System.currentTimeMillis();
        return s;
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        seen(deviceId).lastTemperature = temperature;
    }

    public synchronized void updateAlarm(String deviceId) {
        seen(deviceId).alarms++;
    }

    public synchronized void updateStatus(String deviceId) {
        seen(deviceId);
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        long now = System.currentTimeMillis();
        System.out.printf("%-10s %8s %15s %7s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");
        for (Map.Entry<String, DeviceState> e : devices.entrySet()) {
            DeviceState s = e.getValue();
            System.out.printf(Locale.US, "%-10s %8.2f %15s %7d%n",
                    e.getKey(), s.lastTemperature, "acum " + (now - s.lastSeen) / 1000 + " s", s.alarms);
        }
    }
}
