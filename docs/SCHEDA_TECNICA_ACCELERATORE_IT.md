# Scheda tecnica: l'acceleratore H.264 nell'FPGA di MiSTer MediaCenter

*Versione 3.0, aggiornata il 26 ottobre 2026 con il filtro anti-blocchi (deblocking) nell'FPGA, che non è ancora in una release. In inglese: [TECHNICAL_SHEET_ACCELERATOR_EN.md](TECHNICAL_SHEET_ACCELERATOR_EN.md).*

Questa scheda è per i programmatori che vogliono capire, modificare o migliorare l'acceleratore, anche ripartendo da zero. Spiega:
- come l'acceleratore divide il lavoro fra il processore ARM e l'FPGA del MiSTer;
- quali file servono;
- come si accende;
- come passano i dati;
- come si prova.

I nomi dei file sono relativi alla cartella principale del repository [bonna79/MiSTer_MediaCenter](https://github.com/bonna79/MiSTer_MediaCenter).

---

## 1. Perché esiste e cosa fa

Il MiSTer (DE10-Nano) ha un processore **ARM Cortex-A9 a due core a 800 MHz** e un'**FPGA Cyclone V**. MiSTer MediaCenter (MMC) è fatto così:
- il lettore `mediacenter` gira sull'ARM e decodifica i video con FFmpeg;
- il core del menu `menu_mediacenter.rbf` gira nell'FPGA e mostra le immagini su HDMI e sul CRT a 15 kHz.

Un video **H.264 1920x1080** chiede all'ARM circa **44 ms per immagine**, ma a 23,976 immagini al secondo ce ne sono solo **41,7**: senza aiuto circa il 14% delle immagini non veniva mostrato. Un decoder H.264 completo nell'FPGA non ci sta, e non ne esiste uno libero adatto. La 3.0 quindi **divide il lavoro**:

| Chi | Cosa fa | Perché lì |
| --- | --- | --- |
| ARM (FFmpeg con una nostra modifica) | Legge il file, decifra il flusso H.264 (CABAC/CAVLC), ricava per ogni macroblocco il tipo, i modi intra, i riferimenti, i vettori di movimento e i coefficienti già dequantizzati | È la parte sequenziale, piena di decisioni bit per bit: un processore la fa bene |
| FPGA (il motore nel core del menu) | Ricostruisce i pixel: trasformata inversa 4x4/8x8, predizione intra, compensazione del movimento con filtri a 6 prese e pesi, somma del residuo, filtro anti-blocchi (deblocking) con il calcolo delle forze dei bordi, scrittura dell'immagine in memoria | È la parte con molti calcoli uguali in parallelo: l'hardware la fa bene |
| FPGA (il blocco zero-copy) | Copia l'immagine finita nel framebuffer, restringendola a 1280 o 960 punti | Così l'ARM non tocca più i pixel |

Con questa divisione l'ARM impiega in media **5–15 ms per immagine** in un 1080p a 23,976 immagini al secondo, e l'FPGA circa 20–33 ms nelle scene pesanti, in parallelo con l'ARM. In un 1080p a 29,97 immagini al secondo molto pesante (HTC.mp4) l'ARM impiega in media circa 25 ms su 33,4 e un terzo delle immagini va oltre: il limite lì è l'ARM, non l'FPGA (capitolo 11).

**Limiti di oggi:**
- solo H.264 a 8 bit, 4:2:0, progressivo, largo 1920 punti;
- il filtro anti-blocchi lo fa l'FPGA dalla fase 4 (capitolo 7). Senza acceleratore il lettore lo salta, come prima, nella "modalità leggera" per i video oltre 576 righe;
- tutto il resto (DVD, Video CD, DivX, 720p, H.264 interlacciato o a 10 bit, HEVC) lo decodifica l'ARM come prima.

---

## 2. I file

### Lato ARM (il lettore `mediacenter`)

| File | Cosa contiene |
| --- | --- |
| `tools/mediacenter/ffmpeg-mediacenter.patch` | La modifica a FFmpeg 6.1.2. Aggiunge `libavcodec/h264_mcacc.c` e `libavcodec/h264_mcdbk.c` (il comando `A5` del deblocking) e li aggancia al decoder H.264 (`h264_slice.c`, `h264_picture.c`, `h264dec.h`). Quando il lettore la accende, il decoder non ricostruisce più i pixel ma scrive i comandi del motore. Contiene anche `MC_PARSEONLY`, usato solo per le misure. |
| `tools/mediacenter/build_ffmpeg.sh` | Compila FFmpeg per l'ARM del MiSTer e applica la patch. |
| `tools/mediacenter/accel.c` | Lato ARM indipendente dall'hardware: <ul><li>gli "slot" (le immagini nella memoria dell'FPGA);</li><li>il buffer finto dei pixel;</li><li>i comandi di ogni immagine;</li><li>l'ordine di invio con due thread di decodifica;</li><li>le sessioni (salti).</li></ul> |
| `tools/mediacenter/accel_link.c` | Il collegamento con il core: <ul><li>`/dev/mem`;</li><li>la casella postale (mailbox);</li><li>l'anello dei comandi e la gestione delle cache;</li><li>l'attesa delle immagini finite;</li><li>il disegno tramite lo zero-copy;</li><li>le righe di diagnosi.</li></ul> |
| `tools/mediacenter/zerocopy.c` | Il blocco zero-copy lato ARM (mailbox a `0x23F00000`). L'acceleratore lo usa per mostrare i suoi slot. |
| `tools/mediacenter/mediacenter.c` | Il lettore. L'acceleratore è usato in tre punti: <ul><li>`open_decoder()` lo accende con `acc_try()`;</li><li>`yuv_draw()` disegna le immagini con `acc_zc_draw()`;</li><li>`acc_fallback()` e `acc_resume()` tornano al decoder normale e lo riaccendono.</li></ul> |

### Lato FPGA (il core del menu `menu_mediacenter.rbf`)

| File | Cosa contiene |
| --- | --- |
| `fpga/menu/menu.sv` | Il core del menu (quello ufficiale del MiSTer più le aggiunte di MMC). Collega `mc_acc`, `mc_zc`, `mc_bus` e `mc_out` alla porta DDR. |
| `fpga/menu/rtl/mc_acc.sv` | Il ponte fra la memoria e il motore. Svolge sei compiti: <ul><li>legge la mailbox;</li><li>legge la tabella delle pagine;</li><li>legge l'anello dei comandi;</li><li>mette in coda le richieste di memoria del motore;</li><li>scrive la risposta;</li><li>si accorge se la memoria non risponde.</li></ul> |
| `accel/rtl/acc_engine.sv` | Il motore di ricostruzione: legge i comandi e scrive i macroblocchi. |
| `accel/rtl/acc_idct.sv` | Trasformata inversa 4x4 e 8x8. |
| `accel/rtl/acc_resid.sv` | Memoria del residuo di un macroblocco (8 banchi, doppio buffer). |
| `accel/rtl/acc_recon.sv` | Memoria del macroblocco ricostruito, da cui si scrive in DDR. |
| `accel/rtl/acc_mc.sv` | Predizione inter (immagini P e B): filtri, pesi, bi-predizione, lettura delle finestre di riferimento. |
| `accel/rtl/acc_bs.sv` | Le forze dei bordi (bS) del deblocking, calcolate dai comandi `A3` del macroblocco e dei vicini a sinistra e sopra. |
| `accel/rtl/acc_dbk.sv` | Il filtro anti-blocchi: prende il macroblocco finito da `acc_recon`, filtra i bordi e scrive l'immagine in DDR. |
| `accel/rtl/acc_ram.sv` | Blocco RAM a doppia porta che Quartus mette in M10K. |
| `fpga/menu/rtl/mc_zc.sv` | Lo zero-copy: legge Y, U e V da una tabella delle pagine, restringe 3:2 o 2:1 e scrive righe YUYV nel framebuffer. |
| `fpga/menu/rtl/mc_bus.sv` | Divide la porta DDR fra `mc_out` (uscita CRT, sempre per prima), `mc_zc` e `mc_acc` (a turni). |
| `fpga/menu/rtl/mc_out.sv` | L'uscita a 15 kHz per il CRT. Non fa parte dell'acceleratore ma ha la precedenza sulla memoria. |
| `fpga/menu/timing.tcl` | Controlla i tempi anche agli angoli 100 °C e -40 °C. Nel `menu.qsf` l'analisi multi-angolo di Quartus è spenta. |

### Documentazione, prove e compilazione

| File | Cosa contiene |
| --- | --- |
| `accel/doc/comandi.md` | **Il formato dei comandi ARM → FPGA**: è il contratto fra le due parti. |
| `accel/sim/model.py` | Modello Python del motore: ricostruisce le immagini dai comandi e le confronta con FFmpeg. |
| `accel/sim/make_tests.sh` | Crea i video di prova, ognuno pensato per una parte del motore. |
| `accel/sim/accdump.c`, `accel/sim/ffmpeg-accel-dump.patch` | Estraggono comandi e immagini attese da un video con un FFmpeg compilato sul PC. |
| `accel/sim/acctest.c` | Prova sul PC il lato ARM vero (`accel.c`, e con `-DACC_FAKE` anche `accel_link.c` contro un core finto). |
| `accel/sim/tb/` (`tb_engine.sv`, `run_engine.py`) | Banco di prova del motore in Icarus Verilog, confronto bit per bit. |
| `accel/sim/core/` (`tb_core.sv`, `run_core.py`) | Banco di prova di `mc_acc` più il motore, con l'anello e una DDR simulata. |
| `accel/syn/` | Sintesi del solo motore (dimensioni e frequenza). |
| `.github/workflows/fpga.yml` | Compila il core del menu con Quartus 17.0.2 (immagine docker `theypsilon/quartus-lite-c5:17.0.2`). |
| `.github/workflows/accel.yml` | Sintesi del solo motore. |
| `.github/workflows/build.yml` | Compila il Main, FFmpeg con la patch e il lettore; con `[release]` nel messaggio del commit prepara la release. |

---

## 3. La mappa della memoria

L'ARM e l'FPGA condividono la DDR3 da 1 GB. Linux usa i primi 512 MB; il resto è a disposizione dei core. Il core indirizza la DDR a parole da 64 bit: **indirizzo del core = indirizzo fisico / 8** (29 bit).

| Indirizzo fisico | Cosa c'è | Chi scrive | Chi legge |
| --- | --- | --- | --- |
| `0x00000000`–`0x1FFFFFFF` | Memoria di Linux. Ci stanno anche l'anello dei comandi (modo normale) e le immagini del decoder per lo zero-copy senza acceleratore. | ARM | ARM, FPGA (per pagine) |
| `0x22000000`–`0x237BB000` | Framebuffer del Main (3 pagine per il video) | `mc_zc`, ARM | scaler HDMI, `mc_out` |
| `0x23E00000` | **Mailbox di `mc_acc`**: parole 0–4 dal lettore, 16–25 risposta del core. A `+0x1000` la tabella delle pagine dell'anello (una parola per pagina da 4 KB). | ARM / `mc_acc` | `mc_acc` / ARM |
| `0x23F00000` | **Mailbox di `mc_zc`**: parole 0–10 dal lettore, 16–18 risposta. A `+0x1000` le tabelle delle pagine delle immagini del decoder (8 KB l'una). A `+0xC0000` le tabelle dei 64 slot dell'acceleratore (4 KB l'una). | ARM / `mc_zc` | `mc_zc` / ARM |
| `0x2FC00000`–`0x2FFFFFFF` | Anello dei comandi nel modo `MC_ACC_RING=so` (4 MB fuori da Linux) | ARM | `mc_acc` |
| `0x30000000`–`0x3FFFFFFF` | **64 slot da 4 MB**: le immagini ricostruite dal motore | motore | motore (riferimenti), `mc_zc` |

**Un'immagine in uno slot**, con i valori dei byte dall'inizio dello slot:
- Y: righe da 2048 byte (1920 punti usati), 1088 righe;
- U: da `2228224` (`278528 × 8`), righe da 1024 byte;
- V: da `2785280` (`348160 × 8`), righe da 1024 byte, 544 righe.

Sono le costanti `ACC_Y_STRIDE`, `ACC_C_STRIDE`, `ACC_U_OFF` e `ACC_V_OFF` in `accel_link.c` e i parametri `YSTRIDE_L2`, `CSTRIDE_L2`, `UOFF` e `VOFF` di `acc_engine.sv`: vanno cambiati insieme. Lo slot = `0x30000000 + numero × 4 MB`; il motore usa il numero modulo 64.

Attenzione: quando il framebuffer non è attivo, il core del menu pulisce la DDR da `0x24000000` a `0x3FFFFFFF` (`ddram.sv`, `ddr_pause`). L'acceleratore lavora solo con il framebuffer attivo.

---

## 4. Come si accende, passo per passo

1. **Il Main carica il nostro core del menu.** `fpga_io.cpp` carica `/media/fat/linux/menu_mediacenter.rbf` al posto di `menu.rbf`. Se un altro Main (per esempio Physical Disc) rimette il menu ufficiale, `user_io.cpp` lo ricarica, una volta sola per menu. Il core si riconosce dalla stringa di versione `vMCY` nella sua `CONF_STR` (`menu_is_mediacenter()`).
2. **Il lettore chiede un framebuffer YUV.** Scrive in `/dev/MiSTer_cmd` il comando `fb_cmd3 422 0 <larghezza> <altezza> <aspetto×1000> 1000`. Per il 1080p il framebuffer è 1280x540 (o 960x540 nel formato 4:3) in YUYV, a 3 pagine. Il Main imposta lo scaler e mette lo stato `status[5]` (FB) del core a 1. Nel core, `fb_own & FB` abilita `mc_zc` e `mc_acc` (`menu.sv`).
3. **Il lettore apre il decoder.** In `open_decoder()` (`mediacenter.c`), per H.264, HEVC e VP9 oltre 576 righe c'è la **modalità leggera**:
   - niente deblocking sull'ARM (con l'acceleratore lo fa l'FPGA);
   - `AV_CODEC_FLAG2_FAST`;
   - per il 1080p metà delle righe (`half_lines`).

   Poi `zcp_open()` prepara lo zero-copy e `acc_try()` prova l'acceleratore.
4. **`acc_try()` (`accel_link.c`) controlla le condizioni:**
   - codec H.264, `yuv420p`, larghezza 1920, `half_lines`;
   - un vero framebuffer;
   - nessun fallimento precedente in questo file (`acc_file_off`);
   - `MC_NOACC` non impostata.

   Poi:
   - apre la mailbox dello zero-copy (`zcp_map()`);
   - chiede a Linux di non spostare le pagine bloccate (`zcp_pin()`: `vm.compact_unevictable_allowed=0` finché il file suona);
   - chiama `al_open()`.
5. **`al_open()`:**
   - apre `/dev/mem` con `O_SYNC`;
   - mappa la mailbox a `0x23E00000` e i registri della cache L2 (PL310) a `0xFFFEF000`;
   - prepara l'anello (capitolo 6);
   - apre una **sessione**: scrive le parole 2, 3, 4 e 0 della mailbox, svuota le scritture, poi scrive un nuovo numero di sessione nella parola 1.

   Il core risponde con `ACOK` nella parola 16 e lo stesso numero nella parola 17, con 0 byte letti e 0 immagini. La prova si fa due volte, entro 0,3 s ciascuna. Se il core non risponde (un core vecchio o quello ufficiale) l'acceleratore resta spento e il log lo dice.
6. **Il lettore scrive le tabelle delle pagine dei 64 slot** nella mailbox dello zero-copy (`+0xC0000`): servono a `mc_zc` per leggere gli slot come se fossero immagini del decoder.
7. **`acc_attach()` (`accel.c`) aggancia FFmpeg:**
   - imposta `mc_acc_hooks`;
   - `get_buffer2 = acc_get_buffer`;
   - `skip_loop_filter = AVDISCARD_DEFAULT` se il core sa filtrare (parola 20, bit 18) e `MC_NODEBLOCK` non è impostata, altrimenti `AVDISCARD_ALL`. Con `DEFAULT` FFmpeg legge i parametri del filtro ma non filtra nessun pixel: scrive il comando `A5`.

   Il log scrive `diag acceleratore: acceso (..., deblocking nell'FPGA)`.

Da qui ogni immagine decodificata passa dall'FPGA.

---

## 5. Il lato ARM: FFmpeg con la nostra modifica

`h264_mcacc.c` è incluso in `h264_slice.c`. Gli agganci al decoder sono quattro:

| Momento | Funzione | Cosa fa |
| --- | --- | --- |
| Inizio immagine (`h264_frame_start`) | `mcacc_begin()` | Controlla che l'immagine sia progressiva, non MBAFF, 8 bit e 4:2:0 (altrimenti `fail`). Chiede al lettore un buffer (`begin`), che contiene già il comando `A1` con lo slot e le dimensioni in macroblocchi. |
| Inizio slice (`decode_slice`) | `mcacc_slice()` | Scrive `A2`: liste di riferimento (lo slot di ogni immagine di riferimento, chiesto con `slot`) e pesi. |
| Ogni macroblocco | `mcacc_mb()` al posto di `ff_h264_hl_decode_mb()` | Se il deblocking è acceso e la slice è filtrata, scrive prima `A5` (`mcacc_dbk()` in `h264_mcdbk.c`: offset della slice, vicini usati, qp dei bordi). Poi scrive `A3`: tipo, posizione, maschera dei blocchi con coefficienti, modi intra o riferimenti e vettori, poi i coefficienti non nulli. Fa sull'ARM le trasformate dei DC (intra 16x16 e croma), che costano poco, e azzera i blocchi di coefficienti come farebbe la ricostruzione. |
| Fine immagine (`ff_h264_field_end`) | `ff_h264_mcacc_end()` | Scrive `A4` e consegna il buffer (`end`). |

Se `mc_acc_hooks` è `NULL` o il contesto non è quello del lettore, il decoder lavora come l'originale.

Il formato completo dei comandi è in [`accel/doc/comandi.md`](../accel/doc/comandi.md). In breve:
- parole da 32 bit, little endian;
- `A1` inizio immagine, `A2` slice, `A5` deblocking del macroblocco (3 parole, prima dell'`A3`), `A3` macroblocco, `A4` fine, `A0` riempimento (per arrivare a un numero pari di parole, perché il core legge parole da 64 bit);
- un'immagine 1080p è circa **1 MB di comandi**: circa 30 MB/s a 24 immagini al secondo.

### `accel.c`: immagini, slot e ordine

- **Slot.** Ogni immagine che FFmpeg alloca (`acc_get_buffer`) prende uno dei 64 slot liberi (una maschera di bit atomica). Lo slot resta occupato finché FFmpeg o il lettore tengono l'immagine; quando l'`AVBuffer` viene liberato lo slot torna libero. Lo slot non è l'indice del DPB di FFmpeg: un posto del DPB può essere riusato mentre il lettore mostra ancora l'immagine.
- **Pixel finti.** Tutte le immagini puntano a un unico buffer di pixel condiviso (`acc.dummy`). FFmpeg ci scrive solo per dipingere di grigio le immagini mancanti; nessuno lo legge.
- **Due thread di decodifica.** Due immagini possono essere scritte insieme. Ogni immagine riceve un numero in ordine di decodifica (`seq`) quando inizia. Un'immagine finita aspetta finché tutte quelle prima non sono state inviate: le immagini arrivano all'FPGA sempre in ordine. Al massimo 16 immagini sono in scrittura nello stesso momento (`ACC_INFLIGHT`).
- **Sessioni.** A ogni salto (`acc_restart`) il numero di sessione cresce. Le immagini della sessione vecchia ancora in scrittura vengono scartate.
- **Fallimento.** Ogni problema chiama `acc_fail()` con il motivo. Il lettore lo vede e torna al decoder normale (capitolo 9).

---

## 6. Il collegamento ARM ↔ FPGA (`accel_link.c`)

### La mailbox di `mc_acc` (a `0x23E00000`, parole da 32 bit)

| Parola | Scritta da | Significato |
| --- | --- | --- |
| 0 | ARM | `0x4341434D` ("MCAC") mentre il lettore è presente; 0 per fermare il core |
| 1 | ARM | numero di sessione: un valore nuovo azzera il motore e fa ripartire l'anello |
| 2 | ARM | indirizzo fisico della tabella delle pagine dell'anello |
| 3 | ARM | pagine dell'anello (1–1024) |
| 4 | ARM | puntatore di scrittura: byte scritti nell'anello dall'inizio della sessione (multiplo di 8) |
| 16 | core | `0x4B4F4341` ("ACOK") |
| 17 | core | sessione in corso |
| 18 | core | byte letti dall'anello: l'ARM può riscriverli |
| 19 | core | immagini finite |
| 20 | core | bit 0: comando inatteso; bit 1: la memoria non ha risposto; bit 15:8: ultimo slot; bit 16: `mc_zc` sa restringere 2:1; bit 18: il core fa il deblocking (comando `A5` da 3 parole) |
| 21 | core | colpi di clock fra le ultime due immagini finite |
| 22 | core | di questi, quelli senza comandi da leggere |
| 23 | core | di questi, quelli in attesa della memoria |
| 24 | core | macroblocchi fatti |
| 25 | core | colpi di clock dall'inizio della sessione |

Il core legge la mailbox ogni 1024 colpi di clock (9,5 µs a 108 MHz). Quando vede un nuovo numero di sessione la rilegge (i valori scritti prima del numero), aspetta che finiscano i trasferimenti in corso, azzera il motore e legge la tabella delle pagine.

### L'anello dei comandi

È un buffer circolare di **4 MB** (1024 pagine da 4 KB). L'ARM scrive, il core legge, e i due contatori (parola 4 e parola 18) dicono quanto spazio c'è. Due modi:

- **`MC_ACC_RING=wc` (normale).** Pagine della memoria del lettore, bloccate (`MAP_LOCKED`). L'indirizzo fisico di ogni pagina viene da `/proc/self/pagemap`. Le stesse pagine vengono poi mappate una seconda volta attraverso `/dev/mem` con `O_SYNC`: per la memoria di Linux questo dà una mappatura *write-combine*, che salta le cache L1 e L2 ma scrive veloce. Prima dell'uso:
  - le righe di cache rimaste dall'azzeramento di Linux vengono spinte fuori (`al_evict`: `__clear_cache` più 32 MB letti e scritti attraverso la L2 da 512 KB);
  - uno schema di prova viene scritto e ricontrollato.
- **`MC_ACC_RING=so`.** 4 MB fuori da Linux a `0x2FC00000`. Niente cache da gestire, ma ogni scrittura aspetta la memoria (*strongly ordered*): serve per confrontare i tempi.

**Coerenza delle cache.** La porta dell'FPGA verso la DDR (f2sdram) non vede le cache dell'ARM. Per questo:
- l'ARM scrive l'anello senza cache;
- dopo ogni pezzo (al massimo 64 KB, `ACC_CHUNK`) esegue `dsb` e chiede al controller L2 PL310 di svuotare il suo store buffer (registro *Cache Sync*, `0xFFFEF730`), a causa dell'erratum 769419;
- solo dopo aggiorna il puntatore di scrittura (parola 4).

Le immagini ricostruite invece stanno fuori da Linux: l'ARM non le legge mai, quindi non c'è nessun problema di cache.

### Inviare, aspettare, disegnare

- **`al_send()`** copia i comandi di un'immagine nell'anello quando c'è posto. Se l'anello resta pieno per più di 1 s, o il core segnala un errore, l'acceleratore si ferma.
- **`al_wait()`** aspetta che il contatore delle immagini finite (parola 19) raggiunga l'immagine da mostrare. Aspetta al massimo 0,5 s: dopo 3 attese a vuoto di fila il lettore torna al decoder normale.
- **`acc_zc_draw()`** compila la mailbox dello zero-copy:
  - pagina del framebuffer;
  - righe da fare (540);
  - tabella dello slot;
  - parole per riga;
  - offset di Y, U e V nello slot (tiene conto del ritaglio di FFmpeg);
  - passo di Y (2 righe, perché si fa metà delle righe);
  - bit 2:1 se il framebuffer è largo 960.

  Poi scrive un nuovo numero di sequenza e aspetta che il core lo ripeta nella parola 18 della sua risposta.

### La mailbox di `mc_zc` (a `0x23F00000`)

| Parola | Significato |
| --- | --- |
| 0 | `0x435A434D` ("MCZC") |
| 1 | sequenza: un valore nuovo avvia un'immagine |
| 2 | pagina del framebuffer (indirizzo fisico) |
| 3 | passo del framebuffer in byte \| righe da fare << 16 |
| 4 | tabella delle pagine (indirizzo fisico) |
| 5 | pagine nella tabella \| parole da 64 bit per riga d'uscita << 16 |
| 6 | offset di Y |
| 7 | byte di Y fra due righe d'uscita |
| 8, 9 | offset di U e di V |
| 10 | byte di U/V fra due righe d'uscita; bit 16: riga croma = riga d'uscita / 2; bit 17: restringe 2:1 invece di 3:2 |
| 16 | colpi di clock spesi (risposta) |
| 17 | 0 fatto, 1 rifiutato (valori sbagliati) |
| 18 | sequenza fatta (scritta per ultima) |

---

## 7. Il lato FPGA

### Come si divide la memoria (`menu.sv`, `mc_bus.sv`)

Il core del menu ha una sola porta DDR (`DDRAM_*`, 64 bit, burst fino a 255 parole).
- Con il framebuffer spento la usa `ddram.sv` per pulire la memoria.
- Con il framebuffer attivo la usa `mc_bus`, con queste precedenze:
  1. **`mc_out`** (uscita CRT) ha sempre la precedenza: se deve leggere una riga, gli altri aspettano la fine del trasferimento in corso;
  2. **`mc_zc`** e **`mc_acc`** si alternano: un turno dura al massimo 16 trasferimenti per `mc_zc` (`ZT`) e 256 per `mc_acc` (`AT`);
  3. i dati letti tornano a chi li ha chiesti: la porta cambia mano solo quando non ci sono letture in corso.

### `mc_acc.sv`: il ponte

- Una **coda di richieste** (8 posti) raccoglie in ordine tutti i trasferimenti: quelli del motore, le letture di mailbox, tabella e anello, la risposta. Una **coda di etichette** (32 posti) dice per ogni lettura a chi vanno i dati e quante parole sono. Così il motore vede una memoria semplice, e i tempi stretti non passano dal motore alla porta DDR.
- **La tabella delle pagine** dell'anello viene letta all'inizio della sessione in una RAM interna.
- **Le letture dell'anello** si fermano alla fine di ogni pagina da 4 KB. Calcolare la fine della pagina e l'ultima pagina un colpo prima è servito a rispettare i tempi a -40 °C.
- **La FIFO dei comandi** (512 parole da 64 bit) dà al motore 32 bit alla volta, prima la metà bassa. Il contatore delle parole prenotate garantisce che le parole in arrivo trovino sempre posto.
- **Se la memoria non risponde** per troppo tempo, il blocco si mette da parte e lo segnala (parola 20, bit 1).
- **La risposta** (parole 16–25) viene riscritta a ogni giro di controllo della mailbox in cui qualcosa è cambiato: byte letti, immagini finite o nuova sessione.

### `acc_engine.sv`: il motore

Il motore lavora su **due macroblocchi alla volta**:
- **fase A** legge i comandi del macroblocco, fa la trasformata inversa dei coefficienti (`acc_idct`) e mette il residuo in `acc_resid`;
- **fase B** fa la predizione (intra nel motore, inter con `acc_mc`), somma il residuo, satura a 0–255 e mette il risultato in `acc_recon`. Da lì il filtro anti-blocchi (`acc_dbk`) lo filtra e lo scrive in DDR nello slot.

I comandi di immagine e di slice aspettano che la fase B sia libera.

- **Predizione intra.** 4x4, 8x8 (con il filtro dei campioni di riferimento), 16x16 e croma, compreso il modo "plane". Le righe e le colonne vicine vengono da memorie di riga interne.
- **Predizione inter (`acc_mc`):**
  - luma a quarti di pixel con il filtro a 6 prese, croma a ottavi bilineare;
  - pesi espliciti e impliciti, bi-predizione;
  - bordi ripetuti fuori dall'immagine.

  Il macroblocco si divide in regioni con un vettore per lista: 16x16 se tutto è uguale, altrimenti 8x8 o 4x4. Mentre una regione viene calcolata, le finestre di riferimento della successiva sono già in lettura. Con due liste, la predizione della lista 0 resta in una piccola RAM e si combina con quella della lista 1 come fa FFmpeg.
- **Velocità misurata in simulazione** (108 MHz):
  - immagini P circa 251 colpi per macroblocco (19 ms per un 1080p);
  - B circa 340 (27 ms);
  - I circa 140 (10,6 ms).

  Un 1080p ha 8160 macroblocchi e il tempo disponibile è 552 colpi per macroblocco. Sul MiSTer, nelle scene pesanti, il motore impiega circa 20–33 ms per immagine, di cui fino a circa 19 ms in attesa della memoria.

### Il filtro anti-blocchi (`acc_bs.sv`, `acc_dbk.sv`, fase 4)

- **Le forze dei bordi (`acc_bs`).** Mentre la fase B ricostruisce un macroblocco, `acc_bs` calcola le 32 forze dei suoi bordi (bS 0–4) come `ff_h264_filter_mb` di FFmpeg per un'immagine progressiva. Usa il comando `A3` del macroblocco e quelli dei vicini a sinistra (registri) e sopra (memoria di riga, una voce per colonna di macroblocchi): intra, blocchi con coefficienti, riferimenti (come slot, dall'`A2`) e vettori. Circa 38 colpi di clock per macroblocco. L'ARM manda solo quello che i comandi non dicono: offset della slice, vicini usati e qp (`A5`, 3 parole).
- **Il filtro (`acc_dbk`).** Prende il posto della vecchia scrittura in DDR. Filtrare un macroblocco cambia fino a 3 punti dall'altra parte dei bordi sinistro e alto, quindi un punto va in memoria solo quando nessun filtro può più cambiarlo: ogni passo scrive una zona spostata di 8 punti a sinistra e 4 righe in su (croma: 8 a sinistra e 2 in su). Tiene un'area di lavoro di 20 righe per 24 colonne di luma (croma 10 per 16 per piano) e le ultime 4 righe di luma (2 di croma) della fila di macroblocchi sopra. Per ogni macroblocco: rotazione dell'area (le righe che escono vanno in DDR o nelle memorie di riga), poi bordi verticali, poi orizzontali, in 4 stadi da 2 colpi.
- La predizione intra usa i punti non filtrati (le memorie di riga del motore), come vuole lo standard; le immagini in DDR, usate come riferimento, sono filtrate.
- Un macroblocco senza `A5` non viene filtrato. Con `MC_NODEBLOCK=1` il lettore non manda `A5` e il risultato è quello della 3.0.

### `mc_zc.sv`: dallo slot al framebuffer

- Legge la mailbox ogni 4096 colpi.
- Per ogni riga d'uscita legge una riga di Y e una di U e V attraverso la tabella delle pagine, a burst di al massimo 32 parole spezzati alle pagine da 4 KB.
- Restringe:
  - **3:2** (1920 → 1280): ogni 3 punti ne danno 2, con pesi 2/3 e 1/3 calcolati in logica e non nei DSP, che servono allo scaler HDMI;
  - **2:1** (1920 → 960): media di due punti.
- Scrive la riga YUYV nel framebuffer. Ha due buffer di riga che si alternano.

### Dopo: colori e uscite

Il framebuffer YUYV 4:2:2 viene convertito in RGB dentro il core (formato del framebuffer, bit 5). Poi:
- lo scaler del MiSTer lo porta alla risoluzione dell'HDMI;
- `mc_out` lo ricampiona a 720 punti per il CRT a 15 kHz (480i/576i).

Il lettore cambia pagina con `fb_cmd3 page <n>`.

---

## 8. Il viaggio di un'immagine

1. Il thread video del lettore manda un pacchetto a FFmpeg.
2. Uno dei due thread di FFmpeg inizia l'immagine:
   - `acc_get_buffer` dà uno slot;
   - `mcacc_begin` scrive `A1`;
   - per ogni slice `A2`, per ogni macroblocco `A5` (se il deblocking è acceso) e `A3`, alla fine `A4`.
3. `acc_end` mette l'immagine in fila. Quando tocca a lei, `al_send` la copia nell'anello e avanza la parola 4.
4. `mc_acc` vede che il puntatore è avanzato, legge l'anello a burst e riempie la FIFO dei comandi.
5. Il motore ricostruisce i macroblocchi, il filtro anti-blocchi li filtra e li scrive nello slot. Per i riferimenti legge gli slot delle immagini precedenti.
6. A fine immagine `mc_acc` aumenta la parola 19 e scrive i tempi.
7. Quando l'immagine deve essere mostrata, `yuv_draw` chiama `acc_zc_draw`:
   - `al_wait` aspetta la parola 19;
   - la mailbox di `mc_zc` riceve i dati dello slot;
   - `mc_zc` scrive la pagina del framebuffer libera.
8. Il lettore mostra la pagina con `fb_cmd3 page`, al momento giusto per l'audio.
9. Quando FFmpeg e il lettore lasciano l'immagine, lo slot torna libero.

---

## 9. Salti, errori e ritorno al decoder normale

- **Salto avanti o indietro:** `acc_seek()` → `al_restart()`. Nuova sessione di `accel.c` e del core; le immagini della sessione vecchia vengono scartate (nel log "scartate dopo un salto").
- **Fallimento:**
  - formato non 8 bit 4:2:0;
  - dimensioni cambiate;
  - slot finiti;
  - anello pieno per oltre 1 s;
  - memoria che non risponde;
  - comando inatteso;
  - 3 immagini di fila non finite in 0,5 s;
  - zero-copy che rifiuta o non risponde.

  Al ciclo successivo `acc_fallback()` chiude il decoder, ne apre uno normale e riparte dallo stesso istante (`media_seek`). Nel log: `diag acceleratore: spento per questo video a ... s (motivo)`.
- **Formato non disegnabile** (per esempio un framebuffer largo 720 per un formato scelto con Start): l'acceleratore si spegne solo finché dura (`acc_file_off = 2`). Quando il formato torna disegnabile per almeno 1 s, `acc_resume()` lo riaccende.
- **`MC_NOACC=1`** in `/media/fat/linux/mediacenter.ini`: l'acceleratore non si accende mai.
- **`MC_NODEBLOCK=1`**: l'acceleratore resta acceso ma l'FPGA non filtra (come la 3.0). Serve solo per i confronti.
- **Core senza filtro** (per esempio quello della 3.0): il bit 18 della parola 20 manca, il lettore non manda `A5` e il log dice `deblocking no (core senza filtro)`.
- **Core senza acceleratore:** la sessione non riceve risposta e il lettore usa il decoder normale.

---

## 10. Diagnosi nel log

Con la diagnosi accesa (normale, `MC_DIAG=0` la spegne) il file `/tmp/mediacenter.log` contiene:

- all'apertura: `diag acceleratore: acceso (anello ..., svuotamento L2 si, sessione N, pronto in X ms, deblocking nell'FPGA)` oppure `spento (motivo)`;
- ogni 10 secondi di video:

  ```
  diag acceleratore 120 s: inviate 240 immagini (980 KB l'una, invio 1.2 ms, 0 attese dell'anello),
  FPGA 33.0 ms tra due immagini (senza comandi 8.0 ms, attesa memoria 19.0 ms),
  disegnate 240 (attesa media 0.5 ms), in ritardo 0, perse 0, slot al massimo 20
  ```

  (esempio con valori indicativi):
  - *invio*: tempo dell'ARM per copiare nell'anello;
  - *FPGA tra due immagini*: dalla parola 21;
  - *senza comandi*: il motore aspettava l'ARM;
  - *attesa memoria*: il motore aspettava la DDR;
  - *in ritardo*: l'immagine non era pronta entro 0,5 s;
  - *perse*: da una sessione vecchia o dopo un errore;
- alla fine un riassunto con gli stessi campi e, se c'è stato, il motivo dello spegnimento.

Le righe `diag decoder` e `diag zero-copy` dicono quanto lavora il resto del lettore.

La riga `diag altri programmi N s: ... (altri in tutto X%)`, ogni 10 s, dice quali altri programmi hanno usato più del 5% della CPU e quanto hanno usato tutti insieme (da `/proc/stat`, compresi i comandi brevi). Serve a capire se un rallentamento viene da fuori. Per questo il lettore mette in pausa MiSTer Remote (`remote.sh`, installato da update_all) mentre un video suona e lo fa ripartire alla fine (righe `in pausa durante il video` e `ripartito dopo il video`). Lo cerca ogni 10 s, perché parte solo quando il wifi è connesso, circa mezzo minuto dopo l'accensione. Nei primi minuti dopo l'accensione cataloga i giochi e usava il 20–30% della CPU. `MC_NOPAUSE=1` lo lascia stare.

---

## 11. Prestazioni e risorse (con il deblocking, ottobre 2026)

| Misura | Valore |
| --- | --- |
| Core del menu con l'acceleratore e il deblocking | 73% della logica (ALM; 55% nella 3.0), 35% dei blocchi di memoria interna (20% dei bit), 85% dei DSP (95 su 112) |
| Frequenza | 108 MHz (`clk_sys`). Con il deblocking i tempi sono stretti: dei seed del fitter 1–9 solo il **seed 4** (`menu.qsf`) li rispetta sia a 100 °C (+0,003 ns) sia a -40 °C (+0,082 ns); HDMI +0,288 ns |
| Motore da solo (sintesi `accel/syn`, fase 3, senza deblocking) | circa 12,9k ALM (31%), 39 DSP, 30 blocchi M10K, Fmax circa 112 MHz |
| One Piece 1080p 23,976 sul MiSTer | 3.0: 238 s senza salti, 5724 immagini dall'FPGA, 0 perse, 0 in ritardo, ARM 5–13 ms. Con il deblocking: 144 s, 0,3% di immagini non mostrate, ARM 7,9 ms in media |
| HTC.mp4, 1080p 29,97, molto pesante | Con il deblocking: 147 s, 12,4% di immagini non mostrate (3.0 senza filtro: 12,4%), FPGA circa 23 ms per immagine, 0 perse, 0 in ritardo; ARM 25,7 ms in media, il 34% oltre 33,4 ms. Spariti i punti viola, verdi e gialli che si vedevano nelle scene di neve senza filtro |

Il filtro nell'FPGA non costa tempo all'ARM: le forze dei bordi le calcola il core. Una prima versione le calcolava sull'ARM (+25–35% di lavoro di lettura del flusso) e portava HTC.mp4 al 53% di immagini non mostrate.

---

## 12. Compilare e provare

**Il core.** `fpga.yml` compila `fpga/menu/menu.qpf` con Quartus 17.0.2 (docker `theypsilon/quartus-lite-c5:17.0.2`). Parte da solo quando cambiano `fpga/**` o `accel/rtl/**`, e stampa il riassunto del fit e i percorsi più lenti di `timing.tcl`. In locale:

```
docker run --rm -v "$PWD:/build" -w /build/fpga/menu theypsilon/quartus-lite-c5:17.0.2 quartus_sh --flow compile menu.qpf
docker run --rm -v "$PWD:/build" -w /build/fpga/menu theypsilon/quartus-lite-c5:17.0.2 quartus_sta -t timing.tcl
```

Ogni compilazione può spostare un po' i percorsi (rumore del fitter): controllare sempre `timing.tcl` agli angoli 100 °C e -40 °C.

**Il lettore.** `build.yml`:
- usa il compilatore ARM `gcc-arm-10.2-2020.11`;
- compila FFmpeg 6.1.2 con `build_ffmpeg.sh`, che applica `ffmpeg-mediacenter.patch` (la cache si rinnova quando cambia la patch);
- poi `make mediacenter FFMPEG=... DVDNAV=...`.

**Le prove in simulazione** (Icarus Verilog e Python), senza MiSTer:
1. Compilare FFmpeg 6.1.2 per il PC (solo C) con `ffmpeg-mediacenter.patch` e `accel/sim/ffmpeg-accel-dump.patch` (vedi `accel/sim/README.md`).
2. `accel/sim/make_tests.sh <cartella> [file 1080p]` crea i video di prova:
   - t1–t6: intra, pesi, CAVLC, coefficienti, 1080p;
   - b1–b4, b6: immagini B.
3. `accdump <video> <prefisso>` scrive `<prefisso>.cmd` (i comandi) e `<prefisso>.rec` (le immagini di FFmpeg senza deblocking). Con `accdump -d` i comandi hanno `A5` e le immagini attese sono filtrate da FFmpeg.
4. `model.py <prefisso>`: il modello Python ricostruisce dai comandi (compreso il deblocking, con le forze calcolate come `acc_bs`) e confronta.
5. `accel/sim/tb/run_engine.py <prefisso> [immagini]`: il motore Verilog, confronto bit per bit.
6. `accel/sim/core/run_core.py <prefisso> [immagini] [pagine dell'anello] [latenza]`: `mc_acc` più il motore con l'anello.
7. `acctest` (con `-DACC_FAKE`): il lato ARM vero (`accel.c`, `accel_link.c`) contro un core finto che legge l'anello a velocità casuali.

Ogni modifica al motore deve restare **identica bit per bit** a FFmpeg su tutti i video di prova prima di andare sul MiSTer.

**Sul MiSTer:**
- `mediacenter --zctest <file>` prova lo zero-copy e le cache;
- `mediacenter --bench-dec` e `--bench-parse` misurano il decoder normale e la sola lettura del flusso (`MC_PARSEONLY`);
- la prova di riferimento è One Piece Episodio 940 1080p, con i log `diag acceleratore` e `diag decoder`.

---

## 13. Se si riparte da zero: l'ordine consigliato

1. **Misurare prima.** Con `--bench-parse` si vede quanto costa all'ARM la sola lettura del flusso. Se non sta comodamente sotto il tempo di un'immagine, l'acceleratore non basta.
2. **Fissare il contratto:** il formato dei comandi (`comandi.md`) e un modello software (`model.py`) che ricostruisce identico a FFmpeg. Tutto il resto si prova contro questo.
3. **Il motore in simulazione:** prima I e P, poi B, sempre bit per bit, poi la sintesi da solo per dimensioni e frequenza.
4. **Il collegamento:** mailbox, anello, sessioni, cache (la parte più delicata sul MiSTer: f2sdram non vede le cache dell'ARM; la PL310 trattiene le scritture).
5. **Il disegno:** lo zero-copy dagli slot al framebuffer, con la precedenza all'uscita CRT.
6. **Il ritorno al decoder normale** per ogni caso che l'FPGA non fa, così il lettore non si blocca mai.

---

## 14. Cosa si può migliorare

- **Il 1080p a 30 immagini al secondo**: nei file molto pesanti (HTC.mp4) circa il 12% delle immagini non viene mostrato perché l'ARM, solo per leggere il flusso H.264, va oltre i 33,4 ms in un terzo delle immagini. L'FPGA ha margine. Si può guadagnare solo alleggerendo il lavoro dell'ARM (per esempio comandi più compatti).
- **Meno attesa della memoria**: fino a circa 19 ms per immagine nelle scene pesanti. Si può migliorare con burst più lunghi per le finestre di riferimento, riuso delle finestre fra regioni vicine e turni di `mc_bus` più adatti.
- **Altri formati:** larghezze diverse da 1920 (1440, 1280), H.264 interlacciato (MBAFF/PAFF), 10 bit; ognuno chiede modifiche ai comandi e al motore.
- **Comandi più compatti**: oggi circa 1 MB per immagine; meno dati vuol dire meno tempo dell'ARM e della memoria.

---

*Licenza: come il resto del progetto (GPL-3.0, vedi `LICENSE`). Sviluppato con l'aiuto dell'intelligenza artificiale (Claude di Anthropic).*
