# Phase 79, Bug-fixes, enhancements, and new hardware support for the VisionFive2

## 0.1 Topic collection, not elaborated yet

### Bugs and enhancements

- USB serial console can easily overrun and then loses information
- Performance of graphics is low (esp. noticeable when plotting from Lisp, or during drawing of the Chess board)
- Long filename support is now available: NVME should now be fully integrated. Switch boot to NVME?
- initialize clock via NTP client (also useful for other boards)
- hotplug support for keyboard/mouse
- other graphics modes, 4k?

### Additional hardware

- The 4 secret AI cores: what can they do? Can we build a minimal Deep Learning framework and some optimized inference framework (target: embeddings and 'cognitive core' style minimal LLM?)
- Audio support
- Graphics hardware, what can it do? Compute?

