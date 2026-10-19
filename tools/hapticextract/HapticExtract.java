import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Decodes Android audio-coupled-haptics ringtones (OGG with ANDROID_HAPTIC=N) using the device's own
 * codecs and writes only the haptic channel(s) as raw signed 16-bit little-endian PCM.
 *
 * Run as shell:
 *   CLASSPATH=/data/local/tmp/hapticextract.dex app_process /system/bin HapticExtract <outDir> <file.ogg>...
 * Prints one line per file: OK <name> rate=<hz> channels=<n> haptic=<n> frames=<n>
 */
public class HapticExtract {
    public static void main(String[] args) throws Exception {
        File outDir = new File(args[0]);
        outDir.mkdirs();
        for (int i = 1; i < args.length; i++) {
            try {
                extract(args[i], outDir);
            } catch (Exception e) {
                System.out.println("ERR " + args[i] + " " + e);
            }
        }
    }

    private static void extract(String path, File outDir) throws Exception {
        MediaExtractor ex = new MediaExtractor();
        ex.setDataSource(path);
        MediaFormat fmt = ex.getTrackFormat(0);
        ex.selectTrack(0);
        int channels = fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
        int rate = fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE);
        int haptic = fmt.containsKey("haptic-channel-count") ? fmt.getInteger("haptic-channel-count") : 0;

        MediaCodec codec = MediaCodec.createDecoderByType(fmt.getString(MediaFormat.KEY_MIME));
        codec.configure(fmt, null, null, 0);
        codec.start();

        String name = new File(path).getName().replaceFirst("\\.[^.]*$", "");
        File out = new File(outDir, name + ".s16");
        BufferedOutputStream os = new BufferedOutputStream(new FileOutputStream(out), 1 << 16);
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        boolean inputDone = false;
        long frames = 0;
        int outChannels = channels;
        byte[] sample = new byte[2];

        while (true) {
            if (!inputDone) {
                int in = codec.dequeueInputBuffer(10000);
                if (in >= 0) {
                    ByteBuffer buf = codec.getInputBuffer(in);
                    int n = ex.readSampleData(buf, 0);
                    if (n < 0) {
                        codec.queueInputBuffer(in, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        inputDone = true;
                    } else {
                        codec.queueInputBuffer(in, 0, n, ex.getSampleTime(), 0);
                        ex.advance();
                    }
                }
            }
            int idx = codec.dequeueOutputBuffer(info, 10000);
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                MediaFormat of = codec.getOutputFormat();
                outChannels = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                if (of.containsKey(MediaFormat.KEY_PCM_ENCODING)
                        && of.getInteger(MediaFormat.KEY_PCM_ENCODING) != 2 /* ENCODING_PCM_16BIT */) {
                    throw new IllegalStateException("unexpected pcm encoding " + of);
                }
                continue;
            }
            if (idx < 0) {
                continue;
            }
            ByteBuffer ob = codec.getOutputBuffer(idx);
            ob.order(ByteOrder.LITTLE_ENDIAN);
            ob.position(info.offset);
            int n = info.size / (2 * outChannels);
            // haptic channels are the last ones; with no tag, assume the last channel
            int hapticCount = haptic > 0 ? haptic : 1;
            for (int f = 0; f < n; f++) {
                for (int c = 0; c < outChannels; c++) {
                    short s = ob.getShort();
                    if (c == outChannels - hapticCount) {   // first haptic channel only
                        sample[0] = (byte) s;
                        sample[1] = (byte) (s >> 8);
                        os.write(sample);
                    }
                }
            }
            frames += n;
            codec.releaseOutputBuffer(idx, false);
            if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                break;
            }
        }
        os.close();
        codec.stop();
        codec.release();
        ex.release();
        System.out.println("OK " + name + " rate=" + rate + " channels=" + channels + "/" + outChannels
                + " haptic=" + haptic + " frames=" + frames);
    }
}
