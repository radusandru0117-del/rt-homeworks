package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String message = inbox.take();
                storage.append(kind, message);   // salvat exact cum a sosit

                try {
                    String[] f = message.split(";", 3);   // deviceId ; timestamp ; restul
                    String deviceId = f[0];

                    switch (kind) {
                        case "telemetry":
                            dashboard.updateTelemetry(deviceId, Double.parseDouble(f[2]));
                            break;
                        case "alarm":
                            dashboard.updateAlarm(deviceId);
                            System.out.println(">>> ALARMĂ: " + message);
                            break;
                        case "status":
                            dashboard.updateStatus(deviceId);
                            System.out.println("<<< " + deviceId + ": " + f[2]);
                            break;
                    }
                } catch (Exception e) {
                    // un mesaj greșit nu oprește thread-ul
                    System.out.println("[" + kind + "] mesaj invalid ignorat: " + message);
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
