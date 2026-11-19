# Protocollo MultiScreen (v1)

Trasporto: TCP. Il tablet si collega a `127.0.0.1:27183` sul tablet stesso; `adb reverse tcp:27183 tcp:<porta>`
lo inoltra al PC sopra il cavo USB (funziona con qualsiasi cavo dati, niente uscita video necessaria).
Ogni tablet ha una porta diversa sul PC (27183, 27184, ...): una sessione e uno schermo per tablet.

Sicurezza: il PC accetta solo connessioni del proprio utente (cioè dal server adb) e l'app deve mandare nel HELLO il
`token` segreto che il PC le passa all'avvio (`am start ... --es token <t>`). Senza token valido il PC ignora tutto
tranne HELLO.

Ogni pacchetto: `u8 type | u32 length (big endian) | payload`.

## Tablet -> PC
| type | nome | payload |
|---|---|---|
| 0x01 | HELLO | JSON: `{proto, token, model, width, height, maxw, maxh, refresh, density, dpi, prefs:{res, fps, bitrate, ui, input, mode, pos}}` (`maxw/maxh` = limite del decoder, `pos` = left/right/top/bottom/auto) |
| 0x02 | TOUCH | `u8 n` poi n volte `u8 id, u8 state(0 down,1 move,2 up), u16 x, u16 y` (x,y normalizzati 0..65535) |
| 0x03 | TOUCH_CANCEL | vuoto |
| 0x04 | POINTER | `u8 buttons(bit0 sx, bit1 dx, bit2 centro), u16 x, u16 y` posizione assoluta |
| 0x05 | SCROLL | `i16 dx, i16 dy` (pixel logici, *10) |
| 0x06 | PING | `u64` tempo client |
| 0x07 | KEYFRAME_REQ | vuoto |
| 0x08 | CLIPBOARD | testo UTF-8 |
| 0x09 | FILE_BEGIN | JSON `{id, name, size, mime}` |
| 0x0A | FILE_DATA | `u32 id` + dati (max 64 KB) |
| 0x0B | FILE_END | `u32 id, u8 ok` |
| 0x0C | FILE_RESULT | JSON `{id, name, ok}` (il tablet ha salvato un file ricevuto) |
| 0x0D | BYE | vuoto: chiudi subito lo schermo |
| 0x0E | PAUSE | vuoto: app in background, smetti di mandare video (lo schermo resta per 30 s) |

Un nuovo HELLO sulla stessa connessione applica le nuove impostazioni (e fa ripartire il video dopo PAUSE):
se cambiano solo fps/qualità riparte solo l'encoder, altrimenti lo schermo virtuale viene ricreato.

## PC -> Tablet
| type | nome | payload |
|---|---|---|
| 0x41 | CONFIG | JSON `{codec:"h264", width, height, fps}` |
| 0x42 | VIDEO | `u8 flags(bit0 keyframe), u64 pts_us`, poi access unit H.264 Annex-B |
| 0x43 | PONG | `u64` eco del ping |
| 0x44 | STATUS | JSON `{msg}` testo da mostrare sul tablet |
| 0x45–0x49 | CLIPBOARD, FILE_BEGIN, FILE_DATA, FILE_END, FILE_RESULT | come sopra, dal PC al tablet (FILE_RESULT aggiunge `where`) |

Preferenze HELLO aggiuntive: `mode` = `extend` | `mirror`; orientamento verticale = `width < height`.
