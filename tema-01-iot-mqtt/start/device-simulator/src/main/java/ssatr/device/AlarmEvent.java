package ssatr.device;

/**
 * Un eveniment de alarmă, pus de taskul periodic în coada taskului sporadic.
 *
 * Pragul este transmis împreună cu temperatura, ca alarma publicată să conțină
 * exact pragul care a declanșat-o, chiar dacă operatorul îl schimbă între timp.
 */
public record AlarmEvent(double temperature, double threshold) {
}
