# Tema 01 – soluție

Topic-urile folosesc prefixul `ssatr/sandru-radu/` (`STUDENT` în ambele `Config.java`).

## Rulare

```bash
cd start/business-service
mvn -q compile exec:java

cd start/device-simulator
mvn -q compile exec:java -Dexec.args="device-1"     # alt terminal: -Dexec.args="device-2"
```

În consola serviciului: `show`, `device-1 SET_INTERVAL 1000`, `device-1 SET_THRESHOLD 25`, `device-1 RESET`, `device-1 STATUS`.

## Decizii de implementare

**TODO 1 – task periodic.** Așteptare absolută pe `System.nanoTime()`: `next += T`, apoi se doarme doar cât a mai rămas până la `next`. Durata jobului nu se adună la perioadă, deci nu apare derivă. `periodMs` se citește la fiecare job, deci `SET_INTERVAL` se aplică de la următoarea eliberare.

**TODO 2 – task sporadic.** Pragul este transmis **împreună cu temperatura**, în `AlarmEvent(temperatura, prag)`, nu citit ulterior din `PeriodicTelemetryTask`. Astfel alarma publicată conține exact pragul care a declanșat-o, chiar dacă operatorul îl schimbă între detecție și publicare. De aceea coada este `BlockingQueue<AlarmEvent>`. Evenimentele care sosesc la mai puțin de `MIN_INTERARRIVAL_MS` de la ultima alarmă publicată sunt respinse.

**TODO 3 – task aperiodic.** Comenzile sunt tratate în ordinea sosirii. O comandă necunoscută sau un parametru invalid produce `ERROR ...` pe `status`, iar taskul continuă.

**TODO 4–6 – serviciul business.** Fiecare mesaj se salvează în CSV, apoi actualizează dashboard-ul; alarmele (`>>>`) și confirmările (`<<<`) se afișează imediat. Dashboard-ul are metode `synchronized`, pentru că este scris de trei thread-uri și citit de consolă. Consola separă `deviceId` de comandă și o publică pe topic-ul `commands` al device-ului.

## Diagrame (`diagrame/`)

Sursă PlantUML (`.puml`) + PNG exportat:

- `diagrama-de-componente`
- `diagrama-de-secventa-alarma`: telemetrie peste prag care produce o alarmă
- `diagrama-de-secventa-set-interval`: operatorul trimite `SET_INTERVAL`, iar device-ul confirmă
- `diagrama-de-timp`: cele trei taskuri pe 10 s, cu o alarmă respinsă și un job aperiodic

## Notă

La rezolvarea temei am folosit asistentul AI Claude (Anthropic) pentru ajutor la cod și la redactarea acestui fișier.
