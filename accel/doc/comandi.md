# Comandi ARM -> FPGA del motore di ricostruzione H.264 (versione 2: anche immagini B)

L'ARM legge il flusso H.264 (CABAC/CAVLC, modi, vettori, coefficienti) e
manda all'FPGA questi comandi. L'FPGA ricostruisce i pixel nella sua memoria.
Parole da 32 bit, little endian. Solo immagini progressive, 8 bit, 4:2:0.

Il formato è verificato con `accel/sim/model.py`: ricostruendo le immagini solo
da questi comandi si ottengono immagini identiche a FFmpeg (video di prova di
`accel/sim/make_tests.sh`).

## Inizio immagine `A1`
- p0: `0xA1` << 24 | slot (0-255): dove l'FPGA scrive l'immagine
- p1: larghezza in macroblocchi | altezza in macroblocchi << 8

## Slice `A2` (liste di riferimento e pesi)
- p0: `0xA2` << 24 | n0 | n1 << 6 | due liste << 12 | pesi << 13 (2 bit,
  1 espliciti, 2 impliciti) | pesi croma << 15 | log2 denominatore luma << 16 |
  log2 denominatore croma << 20
- per ogni riferimento della lista 0 (poi della lista 1), 3 parole:
  - slot | peso luma (9 bit con segno) << 8 | offset luma (8 bit) << 17
  - peso Cb (9 bit) | offset Cb (8 bit) << 9
  - peso Cr (9 bit) | offset Cr (8 bit) << 9

## Macroblocco `A3`
- p0: `0xA3` << 24 | tipo << 16 | mb_y << 8 | mb_x
  - tipo: 0 inter, 1 intra 4x4, 2 intra 8x8, 3 intra 16x16, 4 PCM
- PCM: seguono 96 parole (256 byte Y, 64 Cb, 64 Cr), fine.
- p1: blocchi con coefficienti (bit 0-15 luma 4x4, con trasformata 8x8 i bit
  0, 4, 8, 12; bit 16-19 Cb, 20-23 Cr) | trasformata 8x8 << 24
- intra: p2-p3 modi dei 16 blocchi 4x4 (4 bit ciascuno, numerazione FFmpeg; con
  8x8 i blocchi 0, 4, 8, 12); p4: campioni in alto a destra disponibili (16 bit,
  come FFmpeg topright_samples_available) | in alto a sinistra << 16; p5: modo
  croma | modo 16x16 << 4
- inter: p2-p5 riferimento di ogni 8x8 (bit 0-5 indice lista 0, bit 6 = non
  usata; bit 7-12 lista 1, bit 13 = non usata; bit 16-24 peso implicito della
  lista 0, 9 bit con segno, 32 se non serve); p6-p21 vettori lista 0 dei 16
  blocchi 4x4 (x 16 bit | y 16 bit << 16, quarti di pixel); p22-p37 lista 1
  - un 8x8 che usa una lista sola: come nelle immagini P, con i pesi espliciti
    della sua lista se pesi = 1 (con pesi impliciti nessun peso)
  - un 8x8 che usa le due liste (immagini B): media pesata come FFmpeg,
    (p0 w0 + p1 w1 + (((o0 + o1 + 1) | 1) << d)) >> (d + 1):
    - pesi = 0: w0 = w1 = 1, o = 0, d = 0 (media semplice)
    - pesi = 1: pesi e offset dei due riferimenti, d = denominatore della slice
    - pesi = 2 (impliciti): w0 dal macroblocco, w1 = 64 - w0, o = 0, d = 5,
      uguali per luma e croma
- poi: numero N di coefficienti non nulli e N parole:
  valore (16 bit con segno, già dequantizzato) | posizione (0-63, ordine raster)
  << 16 | blocco (0-15 luma, 16-19 Cb, 20-23 Cr) << 22
  - i DC dei blocchi intra 16x16 e del croma sono già trasformati dall'ARM

## Fine immagine `A4`
- p0: `0xA4` << 24
