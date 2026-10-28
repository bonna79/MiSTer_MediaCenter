# MiSTer MediaCenter (MMC)

**Italiano** · [English](README_EN.md)

> [!WARNING]
> **AVVERTENZA: HDMI e CRT collegati insieme.** Se usi insieme l'uscita HDMI verso un televisore LCD e l'uscita VGA→SCART verso un CRT, collega **MiSTer, televisore LCD e CRT alla stessa multipresa elettrica**. Con i due televisori su prese diverse, accendere il CRT mentre il MiSTer è acceso può far passare uno sbalzo elettrico attraverso il MiSTer, tramite le masse dei cavi. È successo in una prova: l'HDMI è diventato verde per qualche secondo, poi l'immagine è sparita su tutte e due le uscite (l'audio continuava) e il pad non rispondeva più; è servito riavviare. Se non puoi usare la stessa multipresa, **accendi il CRT prima del MiSTer**.

<p align="center"><img src="docs/mmc.png" alt="MiSTer Media Center Ver. 3.5 1080p: DVD, MP3, DivX, Video CD, Super Video CD, CD Audio" width="720"></p>

> Questo progetto è stato sviluppato con l'aiuto dell'intelligenza artificiale: il codice e la documentazione sono stati scritti con Claude, l'IA di Anthropic, e provati da rgm su un MiSTer vero.

## Cos'è

MiSTer MediaCenter (MMC) trasforma il MiSTer FPGA (DE10-Nano) in un lettore multimediale per dischi e file video e audio. Un solo programma, con gli stessi comandi, riproduce DVD-Video, Video CD, Super Video CD, video DivX/Xvid/MP4 e musica.

È pensato per i formati a **risoluzione standard**, quella di un DVD o più bassa, e per due tipi di schermo:

- **televisori LCD**, con il collegamento HDMI, alla risoluzione del televisore;
- **soprattutto televisori CRT**, con l'uscita analogica VGA a **15 kHz**: il video arriva al CRT con il suo segnale originale, 576i per i DVD PAL e 480i per gli NTSC, senza conversioni, e l'immagine si adatta al televisore con Regola CRT.

Le due uscite funzionano insieme: lo stesso video va al televisore LCD in HDMI e al CRT in VGA.

Il processore ARM del MiSTer legge e decodifica dischi e file. L'FPGA si occupa dell'uscita video: ingrandisce l'immagine per l'HDMI e genera il segnale a 15 kHz per il CRT.

Legge dal lettore ottico USB, dalla scheda SD, dalle chiavette USB e dalle cartelle di rete.

Il pacchetto è formato da queste parti:

| File | Dove va sulla SD | Cosa fa |
| --- | --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/` | Il Main, il programma principale del MiSTer, usato nel menu. Quando inserisci un disco lo riconosce: dischi video e DVD dati li apre con il lettore, i giochi li avvia con il core giusto |
| `mediacenter` | `/media/fat/linux/` | Il lettore vero e proprio, che gira sull'ARM |
| `mediacenter.sh` | `/media/fat/Scripts/` | Lo script che compare in **Scripts**: apre il lettore per scegliere file da SD, USB o rete |
| `menu_mediacenter.rbf` | `/media/fat/linux/` | Il core del menu di MMC: il menu normale del MiSTer più la conversione dei colori, l'uscita a 15 kHz per il CRT e, dalla 3.0, l'acceleratore H.264 per il 1080p |
| `libdvdcss.so.2` | `/media/fat/dvdcss/` | La libreria per leggere i DVD protetti |
| `MMC_MiSTer_ini.txt` | `/media/fat/` | Le righe da copiare in `MiSTer.ini` (non è un `MiSTer.ini`: copia solo le righe) |

Il vecchio `vcd_player` delle prime versioni non serve più: il lettore Video CD è dentro `mediacenter`.

## Novità della versione 3.5

La 3.5 completa l'acceleratore della 3.0: l'immagine 1080p è più pulita, i video 1080p a 30 immagini al secondo sono fluidi e il menu sopra il video non li rallenta più.

- **Filtro anti-blocchi (deblocking) nell'FPGA:** il core del menu ora fa anche il filtro che l'H.264 usa per nascondere i bordi dei blocchi, e calcola da solo quanto filtrare ogni bordo: all'ARM non costa niente. Spariti i punti viola, verdi e gialli che si vedevano nelle scene veloci con la neve. `MC_NODEBLOCK=1` in `mediacenter.ini` lo spegne (solo per i confronti).
- **1080p a 30 immagini al secondo fluido:** HTC.mp4 (1080p 29,97, 4,5 Mbit/s) passa dal 12,4% di immagini non mostrate della 3.0 allo 0,1%. Il lettore riusa prima le pagine dello schermo, scrive i comandi direttamente nella memoria che legge l'FPGA e tiene 24 immagini decodificate in anticipo; all'inizio e dopo un salto aspetta di averle pronte (al massimo un secondo).
- **Menu sopra il 1080p senza scatti:** la barra, il menu e le scritte del lettore sopra un video accelerato li mescola il core del menu mentre copia l'immagine. Prima lo faceva l'ARM, circa 5 ms per immagine, e a 30 immagini al secondo con il menu aperto qualche immagine veniva scartata. Ora all'ARM restano 0,3 ms.
- **Subito dopo l'accensione il video aspetta la fine dell'avvio:** nei primi 30–40 secondi la MiSTer esegue ancora i suoi script di avvio, e un 1080p partito in quel momento scattava. Ora il lettore mostra "La MiSTer sta finendo l'avvio" con i secondi trascorsi e fa partire il video da solo quando l'avvio è finito (al massimo 60 secondi); B torna indietro. Se la MiSTer è accesa da più tempo il video parte subito. `MC_NOBOOTWAIT=1` toglie l'attesa.
- **Il video ha la precedenza sugli altri programmi** mentre suona; alla fine tutto torna come prima. `MC_NOPRIO=1` la toglie.
- **MiSTer Remote in pausa durante il video:** nei primi minuti dopo l'accensione cataloga i giochi e usava il 20–30% della CPU. Il lettore lo mette in pausa mentre un video suona e lo fa ripartire alla fine. `MC_NOPAUSE=1` lo lascia stare.
- **Diagnosi nel log:** il bitrate del video, i picchi del decoder, il tempo per disegnare ogni immagine, gli altri programmi che usano la CPU e i comandi mescolati dal core.
- **Nuova immagine** con la scritta Ver. 3.5, e il numero della build in basso.

**Fin dove arriva il 1080p.** Con l'acceleratore l'ARM deve ancora leggere il flusso H.264, e il suo lavoro cresce con il bitrate del video. Dalle prove, i limiti per un video fluido sono circa **6 Mbit/s a 30 immagini al secondo**, **8,5–9 Mbit/s a 25** e **9–9,5 Mbit/s a 24**: sono stime ricavate dalle misure, non provate una per una. Oltre, il video va ma con molte immagini non mostrate (un 1080p a 30 immagini al secondo e 11 Mbit/s ne perde circa due su tre; l'audio resta pulito). Per andare oltre l'ARM servirebbe leggere il flusso nell'FPGA: è un obiettivo per la versione 4.0.

Provata su un MiSTer vero il 25–28 ottobre 2026, su CRT Sony Trinitron (SCART RGB, 480i) e HDMI insieme:

| Prova | Risultato |
| --- | --- |
| HTC.mp4, 1080p 29,97, 4,5 Mbit/s, 147 s, con il menu aperto più volte | 0,1% di immagini non mostrate (3.0: 12,4%), nessuna immagine persa dall'FPGA; menu pulito su HDMI e CRT |
| HTC.mp4 subito dopo l'accensione | Attesa della fine dell'avvio di 18 s, poi 0,3% di immagini non mostrate (senza attesa: 6,8%) |
| One Piece Episodio 940, 1080p 23,976 | 0,3% di immagini non mostrate, ARM 7,9 ms per immagine in media |
| Scene con la neve di HTC.mp4 | Spariti i punti colorati che si vedevano senza il filtro anti-blocchi |
| 1080p 29,97 a 6 Mbit/s | 0,3–1,4% di immagini non mostrate secondo la prova: è il limite a 30 immagini al secondo |
| 1080p 29,97 a 11 Mbit/s | 64,7% di immagini non mostrate, audio pulito: oltre il limite |
| 720p H.264, One Piece Episodio 938, 166 s | 0,3% di immagini non mostrate, come nella 3.0 (senza acceleratore) |
| DVD PAL, Star Wars Episodio III | 576i 50 Hz sul CRT, menu e film come nella 3.0 |

I dettagli tecnici sono nella [scheda tecnica dell'acceleratore](docs/SCHEDA_TECNICA_ACCELERATORE_IT.md).

## Novità della versione 3.0

> [!WARNING]
> **AVVERTENZA: HDMI e CRT collegati insieme.** Se usi insieme l'uscita HDMI verso un televisore LCD e l'uscita VGA→SCART verso un CRT, collega **MiSTer, televisore LCD e CRT alla stessa multipresa elettrica**. Con i due televisori su prese diverse, accendere il CRT mentre il MiSTer è acceso può far passare uno sbalzo elettrico attraverso il MiSTer, tramite le masse dei cavi. È successo in una prova: l'HDMI è diventato verde per qualche secondo, poi l'immagine è sparita su tutte e due le uscite (l'audio continuava) e il pad non rispondeva più; è servito riavviare. Se non puoi usare la stessa multipresa, **accendi il CRT prima del MiSTer**.

La 3.0 riproduce i video **1080p H.264** fluidi, su HDMI e su CRT: l'FPGA del MiSTer aiuta l'ARM a decodificarli.

- **Acceleratore H.264 nell'FPGA:** il core del menu contiene un motore che ricostruisce le immagini dei video H.264. L'ARM legge il file e decifra il video (la parte che solo un processore sa fare bene), poi manda all'FPGA i comandi di ogni immagine; l'FPGA ricostruisce l'immagine nella sua memoria e la copia da sola nel framebuffer. Prima l'ARM impiegava circa 44 ms per immagine, più del tempo disponibile (41,7 ms a 24 immagini al secondo); ora gliene bastano in media 5–15.
- **1080p fluido:** One Piece 1920x1080 a 23,976 immagini al secondo, nei tratti senza salti, ha 240 immagini mostrate su 240 ogni 10 secondi, contro circa il 14% di immagini non mostrate della 2.8. Panoramiche orizzontali e verticali fluide su HDMI e CRT.
- **Anche nel formato 4:3 con Start:** l'FPGA restringe l'immagine a 1280 punti (Auto e gli altri formati) o a 960 (4:3), e il cambio di formato durante il film non spegne l'acceleratore.
- **Quando si usa:** solo per i video H.264 a 8 bit, 4:2:0, progressivi, larghi 1920 punti. Tutto il resto (DVD, Video CD, DivX, 720p, H.264 interlacciato o a 10 bit, HEVC) va come nella 2.8, con il decoder dell'ARM. Se l'acceleratore trova un problema torna da solo al decoder normale dallo stesso punto del film, e lo scrive nel log. `MC_NOACC=1` in `mediacenter.ini` lo spegne.
- **Diagnosi nel log:** ogni 10 secondi una riga `diag acceleratore` dice quante immagini sono state mandate e disegnate, i tempi dell'ARM e dell'FPGA, l'attesa della memoria e le immagini in ritardo o perse.
- **Chiavetta USB elencata una volta sola:** se il sistema monta la stessa chiavetta su due cartelle (`/media/usb0` e `/media/usb1`), l'elenco delle sorgenti la mostra una volta sola.
- **Video CD giusto sul CRT:** i Video CD da disco ora li riproduce lo stesso lettore dei Super Video CD (FFmpeg). Sul CRT l'immagine 4:3 occupa tutto lo schermo, senza le bande nere ai lati che comparivano prima, i dischi PAL vanno a 576i 50 Hz e con Start si apre il riquadro Formato. Menu del disco e capitoli funzionano come prima. `MC_VCD_OLD=1` in `mediacenter.ini` rimette il vecchio lettore.
- **Nuova immagine** con la scritta Ver. 3.0 1080 P, mostrata 3 secondi solo quando si avvia lo script mediacenter a mano (non quando parte un DVD, un Super Video CD o un Video CD).

Il motore non ha ancora il filtro anti-blocchi (deblocking) dell'H.264: come già nella 2.8 per il 1080p, l'immagine è quella della modalità leggera. Il filtro è il prossimo passo.

Provata su un MiSTer vero il 24 ottobre 2026, su CRT Sony Trinitron (SCART RGB, 480i 59,94 Hz) e HDMI insieme, con One Piece Episodio 940 1080p H.264:

| Prova | Risultato |
| --- | --- |
| Riproduzione continua, 238 s | 5724 immagini disegnate dall'FPGA, nessuna persa né in ritardo; ARM in media 5–13 ms per immagine |
| Prova lunga con salti avanti e indietro, 449 s | 10365 immagini mostrate; nei tratti senza salti 240 su 240 ogni 10 secondi; le poche non mostrate sono tutte subito dopo i salti |
| Cambio di formato con Start (Auto, 16:9, 4:3 e ritorno) | Fluido in tutti i formati, l'acceleratore resta acceso; su 265 s con salti e cambi il 2,6% di immagini non mostrate (attorno ai salti) |
| Scene pesanti (panoramiche, esplosioni) | FPGA al lavoro circa 30 ms per immagine su 41,7, nessuna immagine in ritardo |
| DVD e DivX dopo l'aggiornamento | Come nella 2.8 (l'acceleratore non si accende) |
| Chiavetta USB montata due volte | Elencata una volta sola |
| Video CD PAL da disco sul CRT | Immagine 4:3 a tutto schermo senza bande laterali, riquadro Formato con Start funzionante |

Alla prima riproduzione dopo l'accensione i primi secondi possono scattare un poco (il file viene letto per la prima volta dalla SD); dalla seconda volta no.

## Novità della versione 2.8

La 2.8 dà a ogni film il suo formato giusto sul CRT e permette di cambiarlo durante la visione.

- **Formato giusto per ogni film sul CRT:** il core del menu mette le bande nere alla proporzione vera dell'immagine (4:3, 16:9, 1,85:1, 2,35:1 e le altre), non più solo al 16:9. Le immagini più strette del 4:3 hanno le bande ai lati.
- **Formato e Pan & scan in Regola CRT:** due voci nuove. **Formato**: Auto (quello che dichiara il disco o il file), Pieno (stirato senza bande), 4:3, 16:9, Anamorfico 2,35:1, Cinema 1,85:1; vale anche per l'HDMI. **Pan & scan** (solo CRT): l'immagine riempie l'altezza e i lati vengono tagliati. Si salvano in `mediacenter.ini` (`MC_CRT_ASPECT`, `MC_CRT_PANSCAN`).
- **Riquadro Formato durante il film:** premi **Start** mentre il film va e compare il riquadro Formato. Con ◄ ► (o ▲ ▼) cambi formato e l'immagine cambia subito, su CRT e HDMI; **A**, **Start** o **B** lo chiudono, e si chiude da solo dopo 6 secondi. La scelta resta salvata. Le voci sono Auto, 4:3, 16:9, Cinema 1,85:1, Anamorfico 2,35:1, Pieno e Pan & scan.
- **Elenchi a 720 punti:** gli elenchi e Regola CRT ora usano un'immagine di 720x576 (PAL) o 720x480 (NTSC), la risoluzione giusta per il CRT. Le scritte restano al centro, nei 640 punti di prima, quindi le regolazioni del CRT non cambiano.
- **Corretto: banda colorata in alto sul CRT.** Se il Main rispondeva in ritardo, per esempio subito dopo un cambio di Modo 15 kHz, il lettore passava al disegno di riserva per tutta la sessione e sopra al film poteva restare una banda azzurra o gialla. Ora riprova al video successivo, aspetta di più dopo un cambio di modo e fuori dal film dipinge sempre il nero.
- **1080p:** mentre il lettore riproduce, il Main resta fermo e lascia i due core dell'ARM al decoder; per i video 1920x1080 il core del menu copia da solo l'immagine nel framebuffer (zero-copy, `MC_NOZC=1` lo spegne). Nella 2.8 il 1080p era ancora sconsigliato (circa il 14% di immagini non mostrate): la 3.0 lo rende fluido con l'acceleratore H.264 nell'FPGA.

Provata su un MiSTer vero il 23 e 24 ottobre 2026, su CRT Sony Trinitron (SCART RGB) e HDMI insieme: DVD di prova della geometria con i cerchi rotondi in 4:3 e in 16:9; bande nere corrette anche dopo i cambi di Modo 15 kHz; riquadro Formato durante il film ed elenchi a 720x480; One Piece 1080p con circa il 14% di immagini non mostrate.

## Novità della versione 2.7

La 2.7 porta l'uscita per il CRT allo standard di un vero lettore DVD.

- **CRT a 720 punti, come un lettore DVD:** l'uscita a 15 kHz ora lavora a 13,5 MHz con 720 punti per riga, lo standard dei DVD (ITU-R BT.601). Un DVD arriva al CRT punto per punto, senza essere ristretto; le altre larghezze vengono adattate con un filtro orizzontale che fonde i punti vicini. Gli elenchi restano larghi 640 punti, al centro, con le scritte intere.
- **Comandi del lettore dentro il bordo rosso:** la riga dei comandi, la barra di stato e le domande a schermo restano dentro l'area segnata dal bordo rosso di Regola CRT; se la riga dei comandi non ci sta va su due righe. Il video resta a 720 punti pieni.
- **Modo 15 kHz Auto dagli fps del video:** in Auto il lettore sceglie lo standard dalla velocità del video: 25 o 50 immagini al secondo escono a 576i 50 Hz (PAL), 23,976, 29,97 o 59,94 a 480i 60 Hz (NTSC). I video escono sempre interlacciati, con tutte le righe. I modi fissi scelti in Regola CRT valgono come prima.
- **DVD in YUV:** anche i colori dei DVD ora li converte l'FPGA, come per i file. Sottotitoli e pulsanti dei menu vengono disegnati direttamente in YUV. L'ARM lavora meno: circa 4–6 ms per immagine. Con `MC_NOYUV=1` si torna al disegno in RGB.
- **Video 720p a piena risoluzione:** i video H.264 a 1280x720 ora usano tutte le 720 righe (prima 360, ingrandite dallo scaler). Le scritte sono più nitide su HDMI e su CRT, e il video resta fluido. Solo i video oltre le 720 righe (1080p) usano ancora metà delle righe.
- **DVD dopo un video 720p:** un DVD riprodotto dopo un video 720p, senza riavviare lo script, partiva a metà risoluzione (720x288). Ora parte sempre a 720x576 (PAL) o 720x480 (NTSC).
- **Immagine di MMC all'avvio:** quando apri lo script **mediacenter** compare per 3 secondi l'immagine di MMC (un tasto la salta; `MC_SPLASH=0` in `mediacenter.ini` la spegne). Con un disco inserito il lettore parte subito.
- **`MC_FIELDS=1` consigliato con il CRT:** vedi [Impostazioni del lettore](#4-impostazioni-del-lettore-facoltative).

Provata su un MiSTer vero il 19 e 20 ottobre 2026, su CRT Sony Trinitron (SCART RGB) e HDMI insieme: Star Wars Episodio III e Shrek 2 PAL con menu e comandi dentro il bordo rosso; Star Wars in YUV con 4454 immagini e nessuna saltata; MP4 H.264 720p a 23,976 fps a 1280x720 pieni, 9528 immagini e una sola non mostrata (0,1%), con scritte perfette sul CRT a 480i; DVD dopo il 720p di nuovo a 720x576.

## Novità della versione 2.6

- **Video più larghi dello schermo molto più fluidi:** un MKV H.264 a 1440x810 perdeva il 63,8% delle immagini, ora l'1,4%. Il lettore restringe l'immagine mentre la copia, senza convertire i colori: la conversione la fa l'FPGA, come per i video a 720p.
- **Ritorno da un gioco PSX (o di un'altra console) con il Main di Physical Disc:** se il menu è rimasto al Main di Physical Disc, lo script **mediacenter** lo riconosce e rimette al suo posto il Main di MMC. Come funziona in [Dopo un gioco con il Main di Physical Disc](#dopo-un-gioco-con-il-main-di-physical-disc).
- **Pubblicità e trailer dei DVD senza immagine:** su alcuni DVD (per esempio Shrek 2) dopo la scelta della lingua si sentivano l'audio e i sottotitoli dei trailer, ma l'immagine restava ferma sul menu. Dopo un pezzo del disco con solo audio, il lettore scartava tutte le immagini come "troppo avanti". Ora le mostra a tempo con l'audio. La stessa correzione fa ripartire bene l'animazione dei menu dal secondo giro in poi.
- **Diagnosi delle immagini:** per 30 secondi dopo ogni salto o cambio di parte del disco, il log scrive ogni secondo quante immagini arrivano, quante vengono mostrate e quante scartate, con il motivo.
- Nel pacchetto c'è anche `MMC_MiSTer_ini.txt`, con le righe da copiare in `MiSTer.ini`.

Provata su un MiSTer vero il 18 ottobre 2026: MKV 1440x810 fluido (3763 immagini, 44 non mostrate); Shrek 2 PAL su CRT con le pubblicità visibili (1775 immagini, 2 saltate); Star Wars Episodio III con menu, capitoli e cambi audio come nella 2.5; gioco PSX e poi script mediacenter con il core MMC di nuovo sul CRT.

## Novità della versione 2.5

La 2.5 è dedicata soprattutto ai televisori CRT e ai DVD.

- **Regola CRT:** una nuova schermata, l'ultima voce dello script **mediacenter**, adatta l'immagine al tuo CRT senza aprire il televisore. Regola **Larghezza**, **Posizione orizzontale**, **Altezza**, **Posizione verticale**, **Modo 15 kHz** e **Filtro tremolio**, e mostra la risoluzione **Misurata** sull'uscita. Dettagli in [Regola CRT](#regola-crt-geometria). Questa funzione è stata possibile grazie al lavoro di Umberto Parisi dei [Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici) (vedi [Ringraziamenti](#ringraziamenti)).
- **DVD interlacciati veri sul CRT:** il core del menu cambia immagine solo all'inizio di un semiquadro. I DVD PAL a 576i 50 Hz e gli NTSC a 480i 60 Hz scorrono fluidi, senza righe che saltano e senza immagini spezzate.
- **Elenchi disegnati per il CRT:** l'elenco dei file e Regola CRT sono disegnati a 640 punti con le righe del modo del CRT, con titoli e voci in grassetto. Le scritte restano intere e leggibili anche a 15 kHz.
- **Filtro tremolio** nel core FPGA: le scritte bianche sottili non sfarfallano più sull'uscita interlacciata.
- **Comandi e sottotitoli leggibili sul CRT:** caratteri più grandi, comandi su due righe con una riga per audio e sottotitoli, e i sottotitoli si spostano sopra i comandi quando sono visibili.
- **Righe di diagnosi nel log** (`mediacenter: diag ...`): per ogni scatto dicono quante immagini sono saltate e perché (lettura del disco, decodifica, disegno, audio). Servono a capire i problemi dalle vostre prove. `MC_DIAG=0` in `mediacenter.ini` le spegne.

Correzioni:

- **CRT dopo un gioco:** tornando al menu dopo un core, il MiSTer caricava il menu di serie e il CRT mostrava solo fruscio o l'avviso "modify MiSTer.ini" fino allo spegnimento. Ora il Main di MMC ricarica sempre il suo core del menu.
- **DVD sul CRT con righe che saltavano e immagini spezzate** ogni 10-15 secondi: corretto nel core, che usa l'indirizzo della nuova pagina solo quando è stabile, e nel lettore.
- **Cambio della traccia audio** dal 5.1 al commento a 2 canali: l'immagine si fermava per circa 5 secondi. Ora il video non aspetta mai e il nuovo audio parte dal punto del film.
- **DVD avviato dallo script mediacenter** più lento (18% di immagini saltate): ora il lettore usa tutti e due i core dell'ARM, comunque venga avviato.
- **Piccoli scatti durante il film** (100-200 ms): il lettore chiedeva lo stato del DVD a libdvdnav mentre questa leggeva il disco, e restava fermo ad aspettare. Ora lo stato lo copia chi legge il disco.
- **Menu dei DVD:** la scelta dei capitoli non scatta più, perché i pulsanti evidenziati vengono preparati una volta sola invece che a ogni immagine. Le scritte del menu appaiono subito e complete, invece che al secondo giro della musica: il lettore fa uscire dal decodificatore l'ultima immagine del menu appena il disco segnala la fine della sequenza video, come fanno i lettori da tavolo.
- **Tasti del joypad scritti sopra il menu del DVD:** la console di Linux ora resta nascosta mentre il lettore è aperto.
- **Disco che ripartiva da solo** dopo stop e uscita: il Main ignora il disco nel lettore finché non viene tolto.

Provata su un MiSTer vero il 17 e 18 ottobre 2026, su CRT Sony Trinitron via SCART RGB (anche su un secondo CRT): DVD PAL protetto con menu, scelta dei capitoli e tre tracce audio con il commento; 3709 immagini di film con solo 3 saltate; DivX e MP4.

## Novità della versione 2.1

- **Uscita a 15 kHz per i CRT funzionante:** il video del lettore ora arriva al televisore CRT dalla VGA analogica (provato con cavo SCART RGB su un Trinitron). Con la 2.0 sul CRT si vedeva solo lo sfondo del menu, perché il Main non avvisava il core del menu che c'era un'immagine da mostrare.
- **Main aggiornato** al Main ufficiale del MiSTer del 16 ottobre 2026: correzioni per l'HDMI (riconoscimento del televisore, `vsync_adjust=2`) e tutte le novità per gli altri core.
- Con `vga_scaler=0` non compare più l'avviso "If you see this, then you need to modify MiSTer.ini" sopra il lettore.

Correzioni:

- **CRT senza immagine:** con la 2.0, sull'uscita VGA senza scaler si vedeva solo il fruscio del menu. Il Main scriveva in modo sbagliato il bit che dice al core "c'è un'immagine di Linux da mostrare" (errore del Main ufficiale, corretto il 1° ottobre 2026). Ora il core lo riceve e porta il video al CRT.
- **DivX tagliati a destra su HDMI:** un video 576x432 veniva raddoppiato a 1152 punti, ma con un televisore a 720p il riquadro 4:3 è largo 960 punti e lo scaler, in questa modalità, non sa rimpicciolire: si vedevano solo i primi 960 punti. Ora il lettore prepara l'immagine già della misura del riquadro.
- **Avviso sopra il lettore:** il Main ufficiale mostra sulla VGA un avviso quando c'è il framebuffer di Linux e lo scaler è spento, perché il menu di serie non sa mostrarlo. Il core di MMC invece lo mostra, quindi l'avviso con MMC non compare più.

Provata su un MiSTer vero (16 ottobre 2026): DVD e DivX su HDMI a 720p e su CRT a 15 kHz interlacciato, sia con il Main ufficiale precedente sia con l'ultimo Main ufficiale installato come `/media/fat/MiSTer`, quello usato dagli altri core.

## Come funziona (parte tecnica)

Il MiSTer ha due parti: un processore **ARM** Cortex-A9 a due core a 800 MHz, con Linux, e un'**FPGA** che genera le uscite video. MMC divide il lavoro così: l'ARM legge e decodifica, l'FPGA mostra. Solo per i video H.264 1080p, dalla 3.0, l'FPGA fa anche la parte più pesante della decodifica.

### Lato ARM

**Il Main (`MiSTer_MediaCenter`)** è il Main del MiSTer con in più il riconoscimento dei dischi:

- quando inserisci un disco lo esamina una volta: DVD-Video, Video CD, Super Video CD e DVD dati con file video li apre con il lettore, i giochi li avvia con il core della console;
- riceve dal lettore i comandi `fb_cmd3` (formato e dimensioni del framebuffer, pagina da mostrare, Regola CRT, modo a 15 kHz) e li passa all'FPGA;
- misura il segnale che esce davvero dal core e lo scrive in `/tmp/mediacenter_crt`;
- se trova caricato il menu di serie, per esempio tornando da un gioco, ricarica il core di MMC.

**Il lettore (`mediacenter`)** è un programma unico per dischi e file, che usa tutti e due i core dell'ARM:

- un thread legge il disco: i DVD con libdvdnav e libdvdcss (menu, pulsanti, capitoli, fermi immagine), i Video CD e Super Video CD direttamente dai settori;
- FFmpeg decodifica il video (MPEG-1, MPEG-2, MPEG-4, H.264…) in un thread a parte, e l'audio (AC3, DTS, MPEG, AAC…);
- l'audio va all'uscita audio del MiSTer, e il video si regola sull'orologio dell'audio per restare sincronizzato;
- nei DVD interlacciati il lettore fonde i due semiquadri di ogni immagine (con `MC_FIELDS=1` li tiene separati: il CRT li riceve intatti e sull'HDMI li fonde lo scaler del Main);
- sottotitoli, pulsanti dei menu DVD e comandi a schermo vengono disegnati sopra l'immagine;
- ogni immagine, DVD compresi, viene copiata nel framebuffer, una zona della memoria DDR condivisa con l'FPGA, in formato YUV 4:2:2, senza convertire i colori.

Il framebuffer ha tre pagine: l'ARM scrive la prossima immagine in una pagina mentre l'FPGA ne mostra un'altra, poi dice al Main quale mostrare (`fb_cmd3 page`). Con un video H.264 a 720p l'ARM impiega circa 30 ms per decodificare un'immagine e 5,5 ms per copiarla, su 41,7 ms disponibili a 24 immagini al secondo.

### Lato FPGA (core `menu_mediacenter.rbf`)

È il core del menu del MiSTer con due aggiunte, che leggono lo stesso framebuffer dalla memoria DDR:

- **HDMI:** lo scaler del MiSTer porta l'immagine in YUV fino all'uscita, la converte in RGB alla fine e la ingrandisce alla risoluzione del televisore (`video_mode`) con le proporzioni giuste. La conversione dei colori nell'FPGA toglie all'ARM circa 16 ms per immagine.
- **CRT a 15 kHz (modulo `mc_out`):** genera da solo il segnale per il CRT, indipendente dall'HDMI, a 13,5 MHz con 720 punti per riga come un lettore DVD: 576i o 480i per i video, secondo il DVD PAL o NTSC o gli fps del file, oppure 288p o 240p se forzati in Regola CRT. Converte i colori, porta l'immagine a 720 punti (i DVD punto per punto, le altre larghezze con un filtro che fonde i punti vicini), aggiunge le bande nere ai film 16:9 e applica Regola CRT ridisegnando l'immagine nel nuovo riquadro senza toccare il sincronismo, così il CRT non perde mai l'aggancio. Cambia pagina solo all'inizio di un semiquadro e solo quando l'indirizzo è stabile, così l'immagine non si spezza. Il filtro tremolio fa la media di ogni riga con quella sotto.

- **Acceleratore H.264 (moduli `mc_acc`, `acc_engine`, `mc_zc`, dalla 3.0):** per i video 1080p FFmpeg sull'ARM legge il video (CABAC, vettori di movimento, coefficienti) ma non ricostruisce le immagini: scrive per ogni immagine i comandi del motore (circa 1 MB) in un anello di memoria. Il motore nell'FPGA, a 108 MHz, li legge dalla memoria DDR e ricostruisce le immagini (trasformata inversa, predizione intra, compensazione del movimento con i pesi delle immagini B) in 64 posti da 4 MB della memoria; poi il blocco zero-copy restringe l'immagine a 1280 o 960 punti e la scrive nel framebuffer in YUV. L'ARM non tocca più i pixel. Con il core 3.0 il core del menu usa il 55% della logica, il 17% della memoria interna e l'85% dei moltiplicatori dell'FPGA. Il funzionamento completo, per chi vuole modificarlo o migliorarlo, è nella [scheda tecnica dell'acceleratore](docs/SCHEDA_TECNICA_ACCELERATORE_IT.md) ([English](docs/TECHNICAL_SHEET_ACCELERATOR_EN.md)).

Le due uscite funzionano insieme: la stessa immagine va all'HDMI e alla VGA nello stesso momento. L'audio è uno solo e arriva insieme all'HDMI, all'uscita analogica e a quella ottica.

L'FPGA non ha spazio per un decodificatore H.264 completo: per questo divide il lavoro con l'ARM, che legge il video, e solo per l'H.264 a 8 bit progressivo largo 1920 punti. Gli altri video li decodifica tutti l'ARM.

## Novità della versione 2.0

- **Colori convertiti dall'FPGA:** il core del menu converte i colori da YUV a RGB, lavoro che prima faceva l'ARM. I video MP4 in H.264 a 720p ora sono fluidi: le immagini non mostrate scendono dal 10% a circa il 2%.
- **Audio e video sincronizzati:** il video aspetta l'audio quando serve, con un piccolo rallentamento invece di uno scatto. Il cambio della traccia audio durante il film (per esempio da 5.1 a commento stereo) avviene senza interruzioni.
- **Super Video CD:** dischi con i menu PBC, e ora anche immagini `.bin` + `.cue` copiate sulla SD.
- **Uscita VGA a 15 kHz per i CRT** dal core del menu, insieme all'HDMI.
- **Convivenza con il Main di Physical Disc:** i giochi su disco ora li gestisce il Main di Physical Disc dentro i core delle console; MMC si occupa di dischi video e file.
- **File di impostazioni** `/media/fat/linux/mediacenter.ini` per regolare il lettore senza toccare gli script.
- I video più larghi dello schermo (per esempio 1440x810) ora si vedono interi, ristretti alla larghezza dello schermo.

## Convivenza con MiSTer Physical Disc

MMC funziona insieme al Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) di Anime0t4ku (`MiSTer_Physical-CD`), da installare a parte. Ognuno fa la sua parte:

| Chi | Dove lavora | Cosa fa |
| --- | --- | --- |
| **MiSTer MediaCenter** | Nel menu del MiSTer | Riconosce il disco inserito. DVD-Video, Video CD, Super Video CD e DVD dati li apre con il lettore; i giochi li avvia con il core di `_Physical Disc Cores` |
| **MiSTer Physical Disc** | Dentro i core delle console | Legge il disco del gioco: subcanale Q, protezione LibCrypt della PSX, CD+G per il karaoke sul Mega CD, immagini CHD |

Il MiSTer passa da un Main all'altro da solo, secondo le righe `main=` di `MiSTer.ini`.

### Configurazione per usarli insieme

Servono tutti e due i Main nella cartella principale della SD:

| File | Da dove |
| --- | --- |
| `/media/fat/MiSTer_MediaCenter` | Da questo progetto ([Releases](../../releases)) |
| `/media/fat/MiSTer_Physical-CD` | Da [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases) |

Poi in `/media/fat/MiSTer.ini` aggiungi queste righe (se le sezioni ci sono già, correggi solo la riga `main=`):

```ini
; Menu del MiSTer: MiSTer MediaCenter riconosce i dischi
; e apre DVD, Video CD, Super Video CD e file video
[menu]
main=MiSTer_MediaCenter
PHYSICAL_DISC_BOOT=1

; Core dei giochi su disco (_Physical Disc Cores): il Main
; di Physical Disc legge il disco (PSX, Saturn, Mega CD...)
[A0CD-*]
main=MiSTer_Physical-CD
```

Riavvia il MiSTer. Da quel momento:

1. nel menu lavora MiSTer MediaCenter: inserisci un DVD o un Video CD e parte il lettore; inserisci un gioco e MMC avvia il core giusto;
2. appena parte il core del gioco, il MiSTer passa da solo al Main di Physical Disc, che legge il disco;
3. quando esci dal gioco e torni al menu, il MiSTer torna da solo a MiSTer MediaCenter (se a volte non succede, vedi [Dopo un gioco con il Main di Physical Disc](#dopo-un-gioco-con-il-main-di-physical-disc)).

Attenzione:

- ogni sezione accetta **una sola** riga `main=`: se ce ne sono due vale solo l'ultima, quindi non mettere i due Main nella stessa sezione;
- se il file del Main di Physical Disc sulla SD ha un altro nome, scrivi quello dopo `main=`;
- senza la sezione `[A0CD-*]` i giochi partono ma il disco non viene letto;
- `PHYSICAL_DISC_BOOT=1` è facoltativo: fa partire all'accensione il disco già nel lettore;
- le opzioni del Main di Physical Disc (per esempio `MEGACD_SUBCODE_LOG`) vanno nella sezione `[A0CD-*]`.

Le altre impostazioni sono spiegate in [Installazione](#3-modifica-mediafatmisterini).

Se inserisci un DVD mentre sei in un gioco, esci al menu del MiSTer: lì MMC lo riconosce e lo fa partire.

### Dopo un gioco con il Main di Physical Disc

A volte, uscendo da un gioco (per esempio un gioco PSX con OSD → **Reset/Exit**), il menu resta al Main di Physical Disc invece di tornare a MiSTer MediaCenter. Il Main di Physical Disc carica il menu normale del MiSTer, senza la parte di MMC per il CRT. Te ne accorgi così: sul televisore HDMI si vede il menu, mentre sul CRT compare l'avviso "If you see this, then you need to modify MiSTer.ini".

Dalla versione 2.6 lo script **mediacenter** se ne accorge da solo:

1. lanci **mediacenter** da **Scripts** come sempre;
2. se nel menu sta lavorando un altro Main, lo script scrive per qualche secondo "Riavvio il Main di MiSTer MediaCenter", chiude quel Main e avvia `MiSTer_MediaCenter`;
3. il Main di MMC ricarica il suo core del menu e il CRT torna a posto;
4. **lancia di nuovo mediacenter da Scripts**: questa seconda volta il lettore parte normalmente, sia in HDMI sia sul CRT.

Serve solo questo secondo avvio, nient'altro: niente riavvii del MiSTer e niente modifiche a `MiSTer.ini`. Lo script lo fa solo se in `MiSTer.ini`, nella sezione `[menu]`, c'è `main=MiSTer_MediaCenter`. Nel file `/tmp/mediacenter_main.log` resta la riga `script: menu run by ...` con il Main che ha trovato.

## Cosa riproduce

| Disco o file | Come si avvia | Funzioni |
| --- | --- | --- |
| DVD-Video, anche originali protetti (CSS) | Parte da solo quando inserisci il disco | Menu del disco, capitoli, tracce audio, sottotitoli, avanti e indietro veloce |
| Video CD 1.0, 1.1 e 2.0 | Parte da solo quando inserisci il disco | Menu PBC, capitoli, canale audio |
| Super Video CD | Parte da solo quando inserisci il disco | Menu PBC, capitoli, tracce audio, avanti e indietro veloce |
| Immagini di Super Video CD `.bin` + `.cue` | Da **Scripts → mediacenter**, aprendo il file `.cue` | Come il disco, con i menu |
| DVD dati (DVD-R o DVD+R) con DivX, MP4, MP3… | Quando inserisci il disco si apre da solo l'elenco dei file | Come per i file |
| Video DivX, Xvid, MP4, MKV, MPEG e VOB | Da **Scripts → mediacenter**, scegliendo SD, USB o rete | Tracce audio, capitoli (o salti di 60 secondi), avanti e indietro veloce |
| Immagini DVD `.iso` e cartelle `VIDEO_TS` | Da **Scripts → mediacenter** | Come un DVD |
| Musica MP1, MP2, MP3, Ogg, FLAC, WAV e AAC | Da **Scripts → mediacenter** | I brani di una cartella si ascoltano di seguito, come un album |
| CD audio | Parte da solo, con il core indicato in `AUDIOCD=` oppure con [MiSTer Hi-Fi](https://github.com/Anime0t4ku/MiSTer_Hi-Fi) | |
| Giochi PSX, Saturn, Mega CD, PC Engine CD… | Partono da soli, con i core di `_Physical Disc Cores` | Gestiti dal Main di Physical Disc |

I CD-R con file DivX o MP3 non partono da soli: si aprono da **Scripts → mediacenter → Disco nel lettore**.

## Formati e risoluzioni consigliate

MMC è fatto per i video a risoluzione standard. Queste sono le risoluzioni massime consigliate:

| Formato | Risoluzione massima consigliata | Risultato |
| --- | --- | --- |
| DVD-Video (MPEG-2) | 720x576 (PAL), 720x480 (NTSC) | Perfetto |
| Video CD (MPEG-1) | 352x288 (PAL), 352x240 (NTSC) | Perfetto |
| Super Video CD (MPEG-2) | 480x576 (PAL), 480x480 (NTSC) | Perfetto |
| DivX, Xvid (MPEG-4) | 720x576 | Fluido |
| MP4, MKV in H.264 a 8 bit | 1280x720 | Fluido |
| MP4, MKV in H.264 a 8 bit progressivo | 1920x1080, con l'acceleratore della 3.0 | Fluido |
| Musica | Qualsiasi | Perfetta |

**I video oltre i 720p che non sono H.264 a 8 bit progressivo 1920x1080, quelli in H.264 a 10 bit (Hi10p) e quelli in HEVC/H.265 sono sconsigliati.** Si riproducono, con l'audio sempre fluido, ma il MiSTer non riesce a mostrare tutti i fotogrammi e l'immagine scatta. Misure fatte su un MiSTer vero:

| Video | Fotogrammi non mostrati |
| --- | --- |
| DVD (MPEG-2 720x576) | meno dell'1% |
| H.264 8 bit 1280x720 | 0,1% a piena risoluzione nella 2.7 (1,4–2,5% nelle versioni precedenti, il 10% prima che l'FPGA convertisse i colori) |
| H.264 8 bit 1440x810 | circa 1,4% (era circa 65% fino alla 2.5) |
| H.264 8 bit 1920x1080 | 0% nei tratti senza salti con l'acceleratore della 3.0 (circa 14% nella 2.8, One Piece) |
| H.264 10 bit (Hi10p) 1280x720 | circa 78% (misurato con la versione 1.0) |

Con un video H.264 a 720p l'ARM impiega circa 30 ms per decodificare un'immagine, su 41,7 ms disponibili a 24 immagini al secondo. Per questo la conversione dei colori, che occupava altri 16 ms, è passata all'FPGA: ora l'ARM copia l'immagine in 5,5 ms.

Il limite è nell'hardware, da tutte e due le parti:

- **ARM:** tutta la decodifica avviene sul processore ARM Cortex-A9 a 800 MHz del DE10-Nano, che non ha un decodificatore video hardware. Basta per DVD e DivX, non per l'H.264 in alta definizione.
- **FPGA:** non ha spazio per un decodificatore H.264 completo, e non ne esiste uno libero adatto al MiSTer. Dalla 3.0 ricostruisce le immagini dei video H.264 1080p mentre l'ARM legge il video; per il resto si occupa dell'uscita video (conversione dei colori, ingrandimento per l'HDMI e segnale a 15 kHz).

Sul CRT, poi, l'alta definizione non serve: l'immagine viene comunque riportata a 480 o 576 righe.

Per guardare un video HD conviene convertirlo, per esempio con HandBrake, a risoluzione DVD: codifica H.264, profilo High, 8 bit, oppure Xvid. Non usare H.264 a 10 bit (Hi10p) né HEVC/H.265.

## Uscita video: HDMI e CRT a 15 kHz

- **HDMI (televisore LCD):** lo scaler dell'FPGA ingrandisce il video alla risoluzione impostata con `video_mode` in `MiSTer.ini`, con le proporzioni giuste (4:3, 16:9, 2.35:1 con le bande nere).
- **VGA a 15 kHz (televisore CRT):** il core `menu_mediacenter.rbf` manda il video al CRT con il suo segnale originale. Gli elenchi del lettore escono a 480i (576i con `menu_pal=1` in `MiSTer.ini`); un DVD esce interlacciato a 576i (PAL) o 480i (NTSC), come da un lettore DVD, e con il modo Auto il CRT cambia frequenza da solo; un film in 16:9 viene mostrato con le bande nere sopra e sotto (letterbox).

MMC non ha bisogno di forzare niente: HDMI e CRT funzionano con le impostazioni normali del MiSTer. Basta che nella parte generale `[MiSTer]` di `MiSTer.ini` lo scaler non sia collegato alla VGA:

```ini
[MiSTer]
vga_scaler=0          ; la VGA porta il segnale del core, non quello dello scaler
forced_scandoubler=0  ; il CRT riceve 15 kHz
composite_sync=1      ; solo con cavo SCART (sincronismo composito)
video_mode=0          ; risoluzione dell'HDMI, per esempio 1280x720
```

Nella sezione `[menu]` **non** mettere `vga_scaler=1`, `video_mode`, `direct_video` o altre forzature dello scaler: valgono solo per il menu, dove gira MMC, e prendono il posto delle impostazioni generali. Con `vga_scaler=1` e un `video_mode` a 240 righe, per esempio, sul CRT arriva l'immagine dello scaler, stretta e tagliata ai lati.

Provata da rgm su un televisore Sony Trinitron con cavo SCART RGB: DVD e DivX centrati e interi, interlacciati a 576i; nello stesso momento HDMI a 720p corretto.

### Regola CRT (geometria)

Se il CRT ha la geometria sballata, l'immagine si può stringere, allargare e spostare senza toccare i trimmer del televisore. Apri lo script **mediacenter** e scegli l'ultima voce, **Regola CRT (dimensioni e posizione)**. Compare un'immagine di prova con un bordo rosso: regola **Larghezza**, **Posizione orizzontale**, **Altezza** e **Posizione verticale** finché il bordo tocca i lati dello schermo.

| Tasto | Cosa fa |
| --- | --- |
| ▲ ▼ | Sceglie la voce |
| ◄ ► | Cambia il valore di un passo (L / R di quattro passi) |
| A | Salva in `mediacenter.ini` ed esce |
| B | Esce senza salvare |
| Y | Riporta tutto a zero |

Le regolazioni valgono per il video del lettore sull'uscita a 15 kHz; l'HDMI non cambia. Il core del menu ridisegna l'immagine nel nuovo riquadro senza cambiare il sincronismo, quindi il CRT non perde mai l'aggancio e i colori restano quelli originali. Questa funzione è stata possibile grazie al lavoro di Umberto Parisi (rmonic79) dei Beceri Videoludici: viene dal suo [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust), adattato al modo in cui MMC disegna l'immagine. Seguite il loro canale YouTube, [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).

Nella stessa schermata ci sono quattro voci gestite dal core FPGA del menu:

| Voce | Valori |
| --- | --- |
| **Modo 15 kHz** | **Auto** (576i per i DVD PAL e i video a 25/50 fps, 480i per i DVD NTSC e i video a 23,976/29,97/59,94 fps; elenchi in 480i o 576i secondo `menu_pal`), oppure fisso **576i PAL**, **288p PAL**, **480i NTSC**, **240p NTSC** |
| **Filtro tremolio** | **Auto** (acceso negli elenchi interlacciati, spento sul video e a 288p/240p), **Spento**, **Acceso** |
| **Formato** | **Auto** (quello che dichiara il disco o il file), **Pieno** (stirato senza bande), **4:3**, **16:9**, **Anamorfico 2,35:1**, **Cinema 1,85:1**; vale anche per l'HDMI. Si cambia anche durante il film con **Start** |
| **Pan & scan** | Solo CRT: l'immagine riempie l'altezza e i lati vengono tagliati |

Il filtro fonde ogni riga con quella sotto: le scritte bianche sottili non sfarfallano più sull'uscita interlacciata. Sul video resta spento in Auto perché i comandi e i sottotitoli sono già ammorbiditi dal lettore, e il film resta nitido.

Sotto le voci compare **Misurata**: la risoluzione che il Main misura davvero sull'uscita del core (per esempio `720x576i 15.63 kHz 50.0 Hz`). La stessa riga, con il modo atteso, finisce anche in `/tmp/mediacenter.log`.

Gli elenchi e la schermata Regola CRT usano un'immagine larga 720 punti con le righe del modo scelto: 480 in Auto (576 con `menu_pal=1`), oppure 576, 288, 480 o 240, con le scritte nei 640 punti centrali. Il CRT la mostra punto per punto e le scritte restano intere; in HDMI viene ingrandita. Con `MC_MENU_HD=1` in `mediacenter.ini` gli elenchi tornano a 1280x720 come nella versione 2.1.

## Prove su un MiSTer vero

Prove fatte da rgm su un MiSTer DE10-Nano con lettore USB Pioneer BDR-TD05, uscita HDMI a 1280x720, fino al 20 ottobre 2026; CRT Sony Trinitron via SCART RGB.

| Prova | Risultato |
| --- | --- |
| DVD PAL originale protetto (Star Wars Episodio III) | Fluido. Menu, capitoli, tracce audio, sottotitoli, avanti e indietro veloce funzionanti |
| DVD con tre tracce audio (inglese 5.1, italiano, commento stereo) | 4289 immagini, 2 saltate. Audio sincronizzato con il labiale; cambio di traccia durante il film senza scatti |
| Video CD | Perfetti, partono da soli quando si inserisce il disco |
| Super Video CD (immagine `.bin` + `.cue` dalla SD) | Menu e tutte e tre le voci di prova funzionanti |
| DVD-R dati con file DivX e MKV | L'elenco dei file si apre da solo, video a tutto schermo |
| MP4 H.264 720p (anime, 23,976 immagini al secondo) | Fluido, 1,4–2,5% di immagini non mostrate, audio sincronizzato, colori corretti con la conversione nell'FPGA |
| MKV H.264 1440x810, versione 2.6 | Intero e fluido, audio e video perfetti: 3763 immagini, 44 non mostrate (1,4%; con la 2.5 era il 63,8%) |
| DVD su CRT (VGA 15 kHz, SCART RGB), versione 2.1 | Immagine centrata e intera, 576i; menu del DVD e film corretti |
| DivX 576x432 su HDMI 720p e su CRT, versione 2.1 | Intero su tutte e due le uscite (prima della 2.1 era tagliato a destra su HDMI) |
| Ultimo Main ufficiale (16 ottobre 2026) come `/media/fat/MiSTer`, versione 2.1 | Funziona tutto, nessun problema |
| DVD PAL protetto su CRT a 576i 50 Hz, versione 2.5 | Fluido: 3709 immagini, 3 saltate; salti tra capitoli senza problemi |
| Menu del DVD su CRT, versione 2.5 | Menu e scelta dei capitoli fluidi, scritte subito complete |
| Cambio della traccia audio su CRT, versione 2.5 | Dal 5.1 al commento a 2 canali e ritorno senza scatti |
| DVD avviato dallo script mediacenter, versione 2.5 | Fluido come con il disco inserito |
| Ritorno al menu dopo un gioco, su CRT, versione 2.5 | Il CRT mostra subito il menu di MMC |
| Regola CRT, versione 2.5 | Larghezza, posizioni, altezza, modo e filtro funzionanti; misura 480i 60 Hz negli elenchi e 576i 50 Hz nei DVD PAL |
| DVD PAL protetto con pubblicità dopo la scelta della lingua (Shrek 2), versione 2.6 | Pubblicità con immagine, audio e sottotitoli; poi menu e film. 1775 immagini, 2 saltate |
| Gioco PSX con il Main di Physical Disc, poi script mediacenter, versione 2.6 | Lo script rimette il Main di MMC; al secondo avvio il CRT mostra il lettore |
| Comandi del lettore su CRT con Regola CRT, versione 2.7 | Star Wars e Shrek 2: menu e comandi dentro il bordo rosso, video a 720 punti pieni |
| DVD PAL in YUV su CRT e HDMI, versione 2.7 | Star Wars Episodio III: 4454 immagini, nessuna saltata; menu, sottotitoli e colori corretti |
| MP4 H.264 720p a piena risoluzione, versione 2.7 | 1280x720 su HDMI, 480i sul CRT in Auto: 9528 immagini, una sola non mostrata (0,1%); scritte perfette sul CRT |
| DVD dopo un video 720p, versione 2.7 | Di nuovo a 720x576: 4752 immagini, una saltata |
| Convivenza con il Main di Physical Disc | DVD e Video CD con MMC; un gioco PSX PAL con LibCrypt (The Legend of Dragoon) parte da solo e funziona con il Main di Physical Disc |

Le prove della 3.0 (1080p con l'acceleratore) sono nella sezione [Novità della versione 3.0](#novità-della-versione-30).

Da provare: Super Video CD letto da un disco masterizzato.

## Installazione

### 1. Scarica i file

Dalla pagina [Releases](../../releases) scarica `MiSTer_MediaCenter.zip` ed estrailo nella cartella principale della SD (`/media/fat`). Le cartelle dello zip sono già organizzate come sulla SD. Lo zip contiene tutto quello che serve a MMC: Main, lettore, script, core del menu, la libreria per i DVD protetti e `MMC_MiSTer_ini.txt` con le righe per `MiSTer.ini`. Il Main di Physical Disc, per i giochi, si scarica dal suo progetto. Aggiornali sempre insieme, perché la conversione dei colori nell'FPGA richiede il Main e il core della stessa versione. Dopo aver copiato il Main o il core riavvia il MiSTer.

**Aggiornare dalla 2.8 alla 3.0:** estrai lo zip della 3.0 sopra i file vecchi e riavvia il MiSTer. Servono insieme il lettore `mediacenter` e il core `menu_mediacenter.rbf` della 3.0: l'acceleratore 1080p è nel core, e con un core vecchio il lettore usa il decoder normale come nella 2.8. `MiSTer.ini` e `mediacenter.ini` restano come sono.

La versione più recente si scarica dalla pagina [Actions](../../actions). Apri l'ultima compilazione riuscita (spunta verde) di **Build MiSTer MediaCenter** e scarica `MiSTer_MediaCenter` e `mediacenter_arm`. Il core del menu è nella compilazione **Build MediaCenter menu core**, file `mediacenter_rbf`.

Installa a parte anche il Main di [MiSTer Physical Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc/releases), se vuoi giocare con i dischi originali.

### 2. Controlla dove sono i file

| File | Posizione sulla SD |
| --- | --- |
| `MiSTer_MediaCenter` | `/media/fat/MiSTer_MediaCenter` |
| `mediacenter` | `/media/fat/linux/mediacenter` |
| `mediacenter.sh` | `/media/fat/Scripts/mediacenter.sh` |
| `menu_mediacenter.rbf` | `/media/fat/linux/menu_mediacenter.rbf` |
| `MiSTer_Physical-CD` (Main di Physical Disc) | `/media/fat/MiSTer_Physical-CD` |

Il Main originale (`/media/fat/MiSTer`) non viene toccato.

Per i DVD protetti serve la libreria `libdvdcss.so.2` nella cartella `/media/fat/dvdcss/`. Lo zip la contiene già: è la [libdvdcss di VideoLAN](https://www.videolan.org/developers/libdvdcss.html) (licenza GPL) per ARM, la stessa provata sul MiSTer con i DVD protetti. È la stessa posizione usata da [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) e dal suo script `install_dvdcss`. In alcuni paesi la legge vieta di aggirare la protezione dei DVD: verifica le regole del tuo paese. Senza questo file si leggono solo i DVD non protetti.

### 3. Modifica `/media/fat/MiSTer.ini`

Aggiungi queste righe, oppure correggile se ci sono già:

```ini
[menu]
main=MiSTer_MediaCenter
PHYSICAL_DISC_BOOT=1

[A0CD-*]
main=MiSTer_Physical-CD
```

- **`[menu]`** fa usare MiSTer MediaCenter nel menu del MiSTer, che è dove vengono riconosciuti i dischi.
- **`[A0CD-*]`** fa usare il Main di Physical Disc dentro i core di `_Physical Disc Cores` (PSX, Saturn, Mega CD…). Senza questa riga i giochi partono ma il disco non viene letto, e sullo schermo compare un avviso. Se il file del Main di Physical Disc nella SD ha un altro nome, scrivi quello.
- **Ogni sezione accetta una sola riga `main=`:** se ce ne sono due, vale solo l'ultima. Non mettere tutti e due i Main nella stessa sezione.
- **`PHYSICAL_DISC_BOOT=1`** è facoltativo: all'accensione fa partire il disco che è già nel lettore, come una console accesa con il disco dentro.
- Le opzioni del Main di Physical Disc, come `MEGACD_SUBCODE_LOG` e `MEGACD_CDDA_PACED`, vanno nella sezione `[A0CD-*]`.
- Per l'HDMI e il CRT non servono righe in `[menu]`: vedi [Uscita video](#uscita-video-hdmi-e-crt-a-15-khz) per le impostazioni generali.

Altre impostazioni facoltative, nella sezione `[menu]`:

| Impostazione | Valori | Effetto |
| --- | --- | --- |
| `DVD=` | `MEDIACENTER`, `HYBRID`, `FPGA` | Sceglie cosa apre i DVD-Video. Se manca, usa `mediacenter`; se `mediacenter` non è installato, apre il core [MiSTer_DVD](https://github.com/owenb321/MiSTer_DVD) |
| `AUDIOCD=` | `PSX`, `A0CD-Saturn`, `A0CD-MegaCD`, `MISTERHIFI`, … | Sceglie il programma o il core per i CD audio. Con `A0CD-MegaCD` i CD+G del karaoke si vedono sul Mega CD |
| `PHYSICAL_DISC_MGL=` | `0`, `1` | Con `0` il riconoscimento automatico dei dischi è spento |

Dopo le modifiche riavvia il MiSTer.

### 4. Impostazioni del lettore (facoltative)

Il lettore legge le sue impostazioni dal file di testo `/media/fat/linux/mediacenter.ini`, una per riga. Il file non serve: crealo solo se vuoi cambiare qualcosa.

```ini
# sposta l'audio di 50 millisecondi più tardi
MC_AV_OFFSET=0.05
```

Con un CRT collegato consigliamo questa configurazione: `MC_FIELDS=1` lascia i DVD interlacciati come sono, e le righe `MC_CRT_` le scrive da sola la schermata Regola CRT (questi sono i valori di rgm, ognuno avrà i suoi).

```ini
# DVD interlacciati: semiquadri intatti sul CRT, fusi dallo scaler sull'HDMI
MC_FIELDS=1
# scritte da Regola CRT (nessuna riga MC_CRT_MODE = modo Auto)
MC_CRT_HSIZE=-3
MC_CRT_HPOS=-1
MC_CRT_VSIZE=-8
MC_CRT_VPOS=2
```

| Impostazione | Effetto |
| --- | --- |
| `MC_AV_OFFSET=0.05` | Sposta l'audio più tardi (valore positivo) o più presto (negativo), in secondi. Serve con i televisori che ritardano l'immagine |
| `MC_CRT_HSIZE`, `MC_CRT_HPOS`, `MC_CRT_VSIZE`, `MC_CRT_VPOS`, `MC_CRT_MODE`, `MC_CRT_FILTER`, `MC_CRT_ASPECT`, `MC_CRT_PANSCAN` | Le regolazioni di [Regola CRT](#regola-crt-geometria), scritte dalla schermata stessa |
| `MC_FIELDS=1` | DVD e video interlacciati: il lettore non fonde i due semiquadri. Il CRT li riceve intatti, come da un vero lettore DVD (più nitido e più fluido nel movimento); sull'HDMI li fonde lo scaler del Main, senza pettini. Consigliato con il CRT. Senza questa riga il lettore fonde le righe per tutte e due le uscite |
| `MC_NOYUV=1` | Fa convertire i colori all'ARM invece che all'FPGA |
| `MC_NOACC=1` | Spegne l'acceleratore H.264 della 3.0: i video 1080p li decodifica l'ARM come nella 2.8 |
| `MC_NOBOOTWAIT=1` | Subito dopo l'accensione il video parte senza aspettare la fine dell'avvio della MiSTer |
| `MC_NOPRIO=1` | Il video non ha la precedenza sugli altri programmi |
| `MC_NOPAUSE=1` | MiSTer Remote non viene messo in pausa durante il video |
| `MC_VCD_OLD=1` | Video CD da disco con il vecchio lettore Video CD, come nella 2.8 (sul CRT con le bande ai lati) |
| `MC_NOZC=1` | Video 1080p senza acceleratore: l'immagine la copia l'ARM invece del core del menu (zero-copy spento) |
| `MC_MENU_HD=1` | Elenchi a 1280x720 come nella versione 2.1, invece che disegnati per il CRT |
| `MC_DIAG=0` | Spegne le righe di diagnosi `mediacenter: diag` nel log |
| `MC_SPLASH=0` | Spegne l'immagine di MMC all'avvio dello script |

Le stesse impostazioni si possono dare anche come variabili d'ambiente, come quelle per i test qui sotto.

## Comandi

Si usa il controller, con i tasti del menu del MiSTer.

| Tasto | Nell'elenco dei file | Durante la riproduzione |
| --- | --- | --- |
| A | Apre | DVD: cambia i sottotitoli |
| B | Torna indietro | Stop; nei Super Video CD torna al primo menu |
| Start | | Play; mentre il film va apre il riquadro Formato |
| Select | | Pausa |
| ◄ ► | Pagina precedente/successiva | Capitolo precedente/successivo; senza capitoli salta di 60 secondi; nella musica cambia brano |
| ▲ ▼ | Su e giù | Cambia traccia audio, oppure il canale (stereo, sinistro, destro) |
| L / R | | Indietro / avanti veloce |
| Y | | DVD, Video CD e Super Video CD: menu del disco |
| X | Esce | Esce (conferma con A) |

Nel menu di un DVD la croce sceglie il pulsante, **A** o **Start** lo conferma e **Y** torna al film. Nel menu di un Video CD o di un Super Video CD le frecce ▲▼ scelgono la voce, **A** la avvia e **Start** apre quella predefinita.

## Copia dei dischi su CD-R

Copiare i giochi in file CHD e riscriverli su CD-R, mantenendo il subcanale Q e la protezione LibCrypt della PSX, non è compito di MMC. Se ne occupa [MiSTer-Disc-Tools](https://github.com/bonna79/MiSTer-Disc-Tools), un programma separato che si installa a parte. I CHD con il subcanale si giocano con il Main di Physical Disc.

## Segnalare un problema

I vostri commenti sono molto preziosi: provate MMC con i vostri dischi e televisori e scrivetemi, anche quando funziona tutto. In caso di problemi mandatemi i log.

Apri una segnalazione nella pagina [Issues](../../issues) e allega:

1. cosa hai fatto e cosa è successo: disco o file, titolo, regione (PAL o NTSC);
2. **foto o video dello schermo**, che aiutano molto a capire il problema;
3. i file `/tmp/mediacenter.log`, `/tmp/mediacenter_main.log` e `/tmp/mediacenter_crt`, da copiare prima di spegnere il MiSTer, perché la cartella `/tmp` si svuota a ogni riavvio;
4. il tipo di uscita video (HDMI o VGA a 15 kHz) e le righe di `MiSTer.ini` che hai cambiato.

## Per chi sviluppa

### Variabili per i test

| Variabile | Effetto |
| --- | --- |
| `MC_ARMSCALE=1` | Ingrandisce l'immagine con l'ARM invece che con lo scaler dell'FPGA |
| `MC_NOVSYNC=1` | Disegna senza aspettare la sincronia verticale |
| `MC_SLOWDRAW=1` | Disegna le immagini con il vecchio metodo, riga per riga |
| `MC_DEBUG=1` | Scrive nel log gli eventi della navigazione nei DVD |
| `MC_FULLQUALITY=1` | Disattiva la modalità leggera dei video H.264/HEVC in alta definizione (deblocking spento, oltre le 720 righe metà delle righe ingrandite dallo scaler FPGA e, sopra i 30 fps, solo le immagini di riferimento) |
| `MC_ACC_RING=so` | Acceleratore: l'anello dei comandi in una zona fissa della memoria invece che nelle pagine del lettore (per confrontare i tempi) |
| `MC_NODEBLOCK=1` | Acceleratore: l'FPGA non fa il filtro anti-blocchi, come nella 3.0 (per i confronti) |
| `MC_NOCOREOSD=1` | Acceleratore: i comandi del lettore sopra l'immagine li mescola l'ARM invece del core (per i confronti) |
| `MC_ACC_COPY=1` | Acceleratore: ogni immagine viene copiata nell'anello dei comandi, come prima, invece di essere scritta direttamente (per confrontare i tempi) |
| `MC_TEST=...` | Prova senza schermo né controller: tasti simulati e immagini salvate in `/tmp/mc_<secondi>.ppm`. Con un file `.cue` riproduce un'immagine di Super Video CD come se fosse nel lettore |

Per misurare la velocità di decodifica usa `/media/fat/linux/mediacenter --bench <file>` oppure `--bench /dev/sr0`. Analizza al massimo il primo minuto del file (Ctrl+C lo ferma prima). Se il risultato supera il 100% del tempo reale, il file si può riprodurre.

Durante la riproduzione `mediacenter` imposta il framebuffer di Linux alla dimensione dell'immagine con il comando `fb_cmd3`, che esiste solo in questo Main. Lo scaler poi lo allarga allo schermo con le proporzioni corrette. Le immagini più larghe dello schermo vengono ristrette dall'ARM, perché lo scaler sa solo ingrandire. Con un altro Main l'immagine viene ingrandita dall'ARM.

### Compilazione

Il workflow `.github/workflows/build.yml` compila il Main e il lettore a ogni push con GCC ARM 10.2 (`arm-none-linux-gnueabihf`). Un commit con `[release]` nel messaggio, oppure un tag `v*`, pubblica una release con lo zip; il numero di versione è nel file `VERSION`. Il workflow `.github/workflows/fpga.yml` compila il core del menu (`fpga/menu`) con Quartus 17.0.

In locale `make` compila il Main. `mediacenter` richiede FFmpeg 6.1.2, libdvdread 6.1.3 e libdvdnav 6.1.1, compilati in statico per l'ARM:

```sh
git clone --depth 1 -b n6.1.2 https://github.com/FFmpeg/FFmpeg.git ffmpeg-src
tools/mediacenter/build_ffmpeg.sh ffmpeg-src ffmpeg-arm
curl -fsSL https://download.videolan.org/pub/videolan/libdvdread/6.1.3/libdvdread-6.1.3.tar.bz2 | tar -xj
curl -fsSL https://download.videolan.org/pub/videolan/libdvdnav/6.1.1/libdvdnav-6.1.1.tar.bz2 | tar -xj
tools/mediacenter/build_dvdnav.sh libdvdread-6.1.3 libdvdnav-6.1.1 dvdnav-arm
make mediacenter FFMPEG=$PWD/ffmpeg-arm DVDNAV=$PWD/dvdnav-arm
```

## Ringraziamenti

- **Umberto Parisi** (rmonic79) dei **Beceri Videoludici**: la funzione Regola CRT è stata possibile grazie al suo lavoro su [MiSTer CRT Adjust](https://github.com/rmonic79/MiSTer-CRT-Adjust). Lo ringrazio e vi consiglio di seguire il loro canale YouTube: [I Beceri Videoludici](https://youtube.com/@rmonic79_ibecerivideoludici).
- **Max Riptide**, per le numerose prove fatte e i suggerimenti.
- **mad4gith**, per le idee suggerite per migliorare MMC.

## Provenienza e licenza

MiSTer MediaCenter deriva da [Main_MiSTer](https://github.com/MiSTer-devel/Main_MiSTer), dal core del menu del MiSTer e dal supporto ai dischi fisici di Physical Disc di Anime0t4ku. Usa FFmpeg, libdvdread e libdvdnav. È distribuito con licenza GPL-3.0, come i progetti da cui deriva: vedi [LICENSE](LICENSE) e [NOTICE](NOTICE).
