# Gad — Documentación

Version 0.4.0 · Español

## Elegir una guía

Cada idioma tiene una guía de inicio rápido, una guía de usuario y una guía de desarrollo. Las guías de usuario incluyen veintidós capturas de los widgets reales con el tema claro y en su propio idioma.

| Documento | Fuente | HTML |
| --- | --- | --- |
| Inicio rápido | [Markdown](QUICK_START.es-ES.md) | [HTML](html/quick-start.es-ES.html) |
| Guía de usuario | [Markdown](USER_GUIDE.es-ES.md) | [HTML](html/user-guide.es-ES.html) |
| Guía de desarrollo | [Markdown](DEVELOPER_GUIDE.es-ES.md) | [HTML](html/developer-guide.es-ES.html) |

Las 18 guías están disponibles en Markdown y HTML autónomo.

[Pruebas y escenarios](../TESTING.es-ES.md)

## Otros idiomas

[Français](README.md) · [English (US)](README.en-US.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

## Regeneración {#regeneration}

Las capturas utilizan un motor de demostración, archivos temporales y sesiones aisladas. No controlan las transferencias reales ni el portapapeles del escritorio.

La exportación requiere Python con Markdown. Las capturas también requieren las dependencias nativas de Gad.

```sh
make docs-captures
make docs
make docs-check
```

138 captures · 18 HTML
