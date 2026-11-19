# MultiScreen

**Il tuo tablet Android diventa un secondo schermo del computer, con il cavo USB.**
Colleghi il cavo e lo schermo compare. Stacchi il cavo e sparisce. Basta.

- Va con **qualsiasi cavo USB che trasmette dati**, anche su tablet senza ingresso video.
- **Immagine fluida e senza ritardo**: usa la scheda video del PC e del tablet.
- **Touch e penna** funzionano sul PC. Si può usare il dito anche come mouse.
- **Appunti condivisi**: copi sul PC, incolli sul tablet (e viceversa).
- **File e foto** tra tablet e PC con un tocco.
- **Più tablet insieme**: ognuno diventa uno schermo in più.

---

## Installare (una volta sola, 5 minuti)

### 1. Sul computer

1. Apri il programma **Konsole** (cercalo nel menu, scrivendo «Konsole»).
2. Copia questa riga, incollala in Konsole con **Ctrl + Maiusc + V** e premi **Invio**:

   ```
   curl -fsSL https://raw.githubusercontent.com/Alessandro1234567/multiscreen/main/get.sh | bash
   ```

3. Se chiede la **password**, scrivi quella del computer e premi **Invio**
   (mentre scrivi non si vede niente: è normale).
4. Aspetta la scritta **«Fatto!»**. Puoi chiudere Konsole.

### 2. Sul tablet

1. Apri **Impostazioni → Info tablet** e tocca **7 volte** la voce **«Versione»**
   (Xiaomi: «Versione OS». Samsung: «Informazioni software → Numero build»).
2. Torna indietro, apri **Opzioni sviluppatore** e attiva **«Debug USB»**.
   Se c'è, attiva anche **«Installa tramite USB»**.
3. **Collega il cavo** al computer. Sul tablet compare una domanda:
   spunta **«Consenti sempre»** e tocca **«Consenti»**.

L'app **MultiScreen** si installa e si apre da sola. Dopo pochi secondi hai il secondo schermo.
Nell'app c'è anche una **Guida** con i pulsanti che aprono le impostazioni giuste.

---

## Usare

| Cosa vuoi fare | Come |
|---|---|
| Avere il secondo schermo | Collega il cavo |
| Toglierlo | Stacca il cavo |
| Cambiare le impostazioni (posizione, qualità, dimensione…) | Tocca il tablet con **4 dita** (o fai il gesto «Indietro») |
| Scegliere se lo schermo sta a destra, sinistra, sopra o sotto | Pannello → **Posizione rispetto al PC** |
| Mandare una foto al PC | Sul tablet: **Condividi → MultiScreen**. Arriva in *Scaricati/MultiScreen* |
| Mandare un file al tablet | Sul PC: tasto destro sul file → **Invia al tablet**. Arriva in *Download/MultiScreen* |
| Copiare e incollare | Normalmente: gli appunti sono condivisi |
| Vedere sul tablet lo stesso schermo del PC | Pannello → Modalità **Duplica** |
| Usare più tablet | Collegali tutti: ognuno è uno schermo in più |

---

## Con cosa funziona

- **Computer**: Linux con **KDE Plasma** 5.27 o 6 (sessione Wayland): CachyOS, Arch, Manjaro, EndeavourOS,
  Kubuntu 24.04+, KDE neon, Fedora KDE 40+, openSUSE Tumbleweed, Debian 13 KDE.
  Windows: in arrivo.
- **Tablet e telefoni**: **Android 6** o più recente.
- **Cavo**: qualsiasi cavo USB con i dati (non quelli solo-ricarica).

---

## Se qualcosa non va

| Problema | Soluzione |
|---|---|
| Collego il cavo e non succede niente | Controlla che «Debug USB» sia attivo. Prova un altro cavo o un'altra porta USB |
| Il tablet non chiede «Consenti» | Stacca e ricollega il cavo. Prova a cambiare la modalità USB in «Trasferimento file» |
| L'app non si installa da sola | Attiva «Installa tramite USB», oppure apri sul tablet `Download/MultiScreen.apk`, oppure scaricala da [Releases](../../releases) |
| Immagine a scatti | Pannello → abbassa **Risoluzione** o **Qualità** |
| Scritte troppo piccole o grandi | Pannello → **Dimensione interfaccia** |
| Serve aiuto | In Konsole: `multiscreen status`. Per i dettagli: `multiscreen log` |

Disinstallare: in Konsole `multiscreen uninstall`

---

## Per sviluppatori

- `linux/` programma per il PC in C: schermo virtuale e input con KWin (`zkde_screencast`, `fake_input`),
  cattura PipeWire (DMA-BUF), encoder H.264 hardware VA-API con GStreamer (x264 di riserva), adb, appunti (wl-clipboard), file.
- `android/` app in Java senza librerie esterne (Android 6+). `android/build-apk.sh` la compila senza Gradle.
- `prebuilt/` programmi già compilati. `tools/make-release.sh` li rifà; la GitHub Action li pubblica a ogni tag `v*`.
- `docs/protocol.md` il protocollo tra PC e tablet.

Licenza MIT.
