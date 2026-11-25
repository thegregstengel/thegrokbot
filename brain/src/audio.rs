/// 20 ms of 16 kHz mono 16-bit PCM = 320 samples = 640 bytes.
pub const PCM_FRAME_SAMPLES: usize = 320;
pub const PCM_FRAME_BYTES: usize = PCM_FRAME_SAMPLES * 2;
pub const SAMPLE_RATE_HZ: u32 = 16_000;

/// Computes a perceptual 0..=255 speech energy level from little-endian PCM16 samples
/// to animate the GrokBot face in sync with spoken syllables.
pub fn pcm16_energy(pcm_le: &[u8]) -> u8 {
    let sample_count = pcm_le.len() / 2;
    if sample_count == 0 {
        return 0;
    }

    let mut sum_sq: f64 = 0.0;
    for chunk in pcm_le.chunks_exact(2) {
        let s = i16::from_le_bytes([chunk[0], chunk[1]]) as f64;
        sum_sq += s * s;
    }

    let rms = (sum_sq / sample_count as f64).sqrt();
    // Map typical speech RMS (roughly 500..10000) into a smooth 0..255 curve.
    let norm = (rms / 8000.0).clamp(0.0, 1.0).powf(0.65);
    (norm * 255.0).round() as u8
}

/// Splits arbitrary PCM16LE byte buffers into exact 20 ms frames for smooth Body playback.
pub struct PcmChunker {
    buf: Vec<u8>,
}

impl PcmChunker {
    pub fn new() -> Self {
        Self {
            buf: Vec::with_capacity(PCM_FRAME_BYTES * 4),
        }
    }

    pub fn push(&mut self, data: &[u8]) -> Vec<Vec<u8>> {
        self.buf.extend_from_slice(data);
        let mut frames = Vec::new();
        while self.buf.len() >= PCM_FRAME_BYTES {
            let frame: Vec<u8> = self.buf.drain(..PCM_FRAME_BYTES).collect();
            frames.push(frame);
        }
        frames
    }

    /// Flushes any trailing partial frame padded with silence.
    pub fn flush(&mut self) -> Option<Vec<u8>> {
        if self.buf.is_empty() {
            return None;
        }
        let mut frame = std::mem::take(&mut self.buf);
        // Ensure even byte count and pad to full frame for clean DMA writes.
        if frame.len() < PCM_FRAME_BYTES {
            frame.resize(PCM_FRAME_BYTES, 0);
        }
        Some(frame)
    }

    pub fn clear(&mut self) {
        self.buf.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn silence_has_zero_energy_and_loud_signal_saturates() {
        let silence = vec![0u8; PCM_FRAME_BYTES];
        assert_eq!(pcm16_energy(&silence), 0);

        let mut loud = Vec::with_capacity(PCM_FRAME_BYTES);
        for _ in 0..PCM_FRAME_SAMPLES {
            loud.extend_from_slice(&12_000i16.to_le_bytes());
        }
        assert_eq!(pcm16_energy(&loud), 255);
    }

    #[test]
    fn chunker_emits_exact_20ms_frames() {
        let mut c = PcmChunker::new();
        assert!(c.push(&vec![1u8; 300]).is_empty());
        let out = c.push(&vec![2u8; 400]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].len(), PCM_FRAME_BYTES);

        let tail = c.flush().unwrap();
        assert_eq!(tail.len(), PCM_FRAME_BYTES);
        assert_eq!(tail[0], 2);
        assert_eq!(tail[60], 0);
    }
}
