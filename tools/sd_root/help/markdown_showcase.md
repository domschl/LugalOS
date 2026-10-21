# Markdown & Typography Showcase

Welcome to the **LugalOS Restricted Markdown** demonstration page[^1]. This document demonstrates rich typography, inline formatting, tables, and mathematics.

[TOC]

## 1. Inline Typography & Spans

LugalOS supports mixed proportional typography within paragraphs:
* Standard text with **bold emphasis** and *italic slant*.
* Combined ***bold and italic*** phrasing.
* Inline code snippets like `int main()` or `vfs_open()` with shaded background pills.
* Inline hyperlinks: visit [Shell Guide](shell.gmi) or [CAS Calculus](calculus.gmi).
* Mathematical symbols: $E = mc^2$ and $e^{i\pi} + 1 = 0$.

> [!NOTE]
> This reader runs with zero heap allocations ($O(1)$ SRAM) and zero floating point in kernel mode!

## 2. GFM Alerts & Callouts

> [!TIP]
> Press `t` at any time to toggle the interactive Table of Contents overlay!

> [!WARNING]
> Raw `<script>` or `<div>` HTML tags are rendered literally to protect system invariants.

## 3. Structured Data & Tables

Here is a comparison of hardware presets:

| Target Board | Architecture | Screen Resolution | Color Depth |
| :--- | :--- | :--- | :--- |
| RP2350-Terminal | Hazard3 Dual RV32 | 800x480 | 1-bpp Monochrome |
| ESP32-P4-LCD-7B | Dual RV32 IMC | 1024x600 | 8-bpp Grayscale |
| M5Stack Tab5 | Dual RV32 IMC | 720x1280 | 16-bpp HiDPI |

## 4. Mathematics & Operators

Display math formulas are centered on the page:

$$
\int_{-\infty}^{\infty} e^{-x^2} dx = \sqrt{\pi}
$$

Ordered lists organize procedural steps:
1. Boot the microkernel.
2. Launch the `reader` application.
3. Enjoy book-grade typography!

---

[^1]: Footnote 1: Typeset entirely with freestanding LugalOS fonts in Flash `.rodata`.
