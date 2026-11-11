# Phase 79, Bug-fixes, enhancements, and new hardware support for the VisionFive2

## 0.1 Topic collection, not elaborated yet

### Bugs and enhancements

- USB serial console can easily overrun and then loses information
- Performance of graphics is low (esp. noticeable when plotting from Lisp, or during drawing of the Chess board)
- Long filename support is now available: NVME should now be fully integrated. Switch boot to NVME?
- initialize clock via NTP client
- hotplug support for keyboard/mouse
- other graphics modes, 4k?
- currently on 4k, 2x upscaling seems to be used? Ways to improve font quality?

### Additional hardware

- What matrix multiplications acceleration is available? Stick to int? Can we build a minimal Deep Learning framework and some optimized inference framework (target: embeddings and 'cognitive core' style minimal LLM?)
- Audio support
- Graphics hardware, what can it do? Compute? Driver stack feasible?

### AI tests (host based for starters?)

- 'Micrograd' style toolkit, C-core, Lisp configuration(?). Let `float` sneak in for this one? Evaluate logarithmic int mapping?
- Embeddings: optimized integer-only inference engine specifically tuned for a small model: "Gemini: all-MiniLM-L6-v2 (22M parameters) or bge-small (33M parameters) require very little RAM (~45–130 MB) and rely on dense matrix multiplications (GEMM) over short token sequences"
- Simple LLM inference with optimized integer-only engine: ternary models or quantization
- Model conversion: PTQ (Post training quantization)

