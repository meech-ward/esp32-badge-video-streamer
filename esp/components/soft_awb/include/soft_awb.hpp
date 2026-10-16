// Software auto white balance for OV2640 and OV5640. Samples stream frames
// at reduced scale, finds near-neutral pixels and nudges manual R/G/B gains.
#pragma once
namespace soft_awb {
void start();  // task on core 1, below capture; no task for other sensors
}
