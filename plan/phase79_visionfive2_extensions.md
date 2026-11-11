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
- equivalent of 'identity sector', use additional partition on NVME?

### Additional hardware

- Audio support
- Graphics hardware, what can it do? Compute? Driver stack feasible?
- What matrix multiplications acceleration is available? Stick to int? Can we build a minimal Deep Learning framework and some optimized inference framework (target: embeddings and 'cognitive core' style minimal LLM?)

