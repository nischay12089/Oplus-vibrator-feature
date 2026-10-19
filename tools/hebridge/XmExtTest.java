import android.os.IBinder;
import android.os.Parcel;

/**
 * Root test tool: calls Xiaomi's vendor.hardware.vibratorfeature.IVibratorExt.play() directly.
 *   CLASSPATH=xmext.dex app_process /system/bin XmExtTest <loop> <interval> <amplitude> <i0,i1,...>
 * data[0] (total length) is filled in automatically.
 */
public class XmExtTest {
    static final String HAL = "android.hardware.vibrator.IVibrator/default";
    static final String DESC = "vendor.hardware.vibratorfeature.IVibratorExt";

    public static void main(String[] a) throws Exception {
        IBinder hal = (IBinder) Class.forName("android.os.ServiceManager")
                .getMethod("getService", String.class).invoke(null, HAL);
        if (a[0].equals("stall")) {   // HE pattern of <ms> via IOplusVibrator, then time a OnePlus tap
            IBinder ext = (IBinder) IBinder.class.getMethod("getExtension").invoke(hal);
            int ms = Integer.parseInt(a[1]);
            int[] he = new int[16 * 55];
            int[] ev = {4096, 0, 100, 50, ms, 0, 4, 0, 0, 0, 100, 100, 0, ms - 100, 100, 0, ms, 0, 0};
            System.arraycopy(ev, 0, he, 0, ev.length);
            Parcel p = Parcel.obtain(ext);
            p.writeInterfaceToken("vendor.oplus.hardware.oplusvibrator.IOplusVibrator");
            p.writeInt(1); p.writeInt(0); p.writeInt(255); p.writeInt(0); p.writeIntArray(he); p.writeStrongBinder(null);
            ext.transact(12, p, null, IBinder.FLAG_ONEWAY);
            Thread.sleep(Integer.parseInt(a[2]));
            if (a.length > 3 && a[3].equals("off")) {   // the framework's off() during the HE stream
                long t0 = System.nanoTime();
                Parcel q = Parcel.obtain(hal), r = Parcel.obtain();
                q.writeInterfaceToken("android.hardware.vibrator.IVibrator");
                hal.transact(2, q, r, 0); r.readException();
                System.out.println("off: call blocked " + (System.nanoTime() - t0) / 1000000 + " ms");
                Thread.sleep(150);
            }
            for (int i = 0; i < 2; i++) {
                long t0 = System.nanoTime();
                Parcel q = Parcel.obtain(hal), r = Parcel.obtain();
                q.writeInterfaceToken("android.hardware.vibrator.IVibrator");
                q.writeFloat(1.5f); hal.transact(6, q, r, 0); r.readException();
                q = Parcel.obtain(hal); r = Parcel.obtain();
                q.writeInterfaceToken("android.hardware.vibrator.IVibrator");
                q.writeInt(2); q.writeByte((byte) 2); q.writeStrongBinder(null);
                hal.transact(4, q, r, 0); r.readException();
                System.out.println("tap " + i + ": call blocked " + (System.nanoTime() - t0) / 1000000 + " ms");
                Thread.sleep(150);
            }
            return;
        }
        if (a[0].equals("oneplus")) {   // OnePlus sequence on IVibrator/default: setAmplitude(1+g), perform(id, STRONG)
            Parcel p = Parcel.obtain(hal), r = Parcel.obtain();
            p.writeInterfaceToken("android.hardware.vibrator.IVibrator");
            p.writeFloat(Float.parseFloat(a[1]));
            hal.transact(6, p, r, 0); r.readException();
            p = Parcel.obtain(hal); r = Parcel.obtain();
            p.writeInterfaceToken("android.hardware.vibrator.IVibrator");
            p.writeInt(Integer.parseInt(a[2])); p.writeByte((byte) 2); p.writeStrongBinder(null);
            hal.transact(4, p, r, 0); r.readException();
            System.out.println("perform -> " + r.readInt() + " ms"); return;
        }
        if (a[0].equals("off")) {   // IVibrator.off(): transaction 2
            Parcel p = Parcel.obtain(hal), r = Parcel.obtain();
            p.writeInterfaceToken("android.hardware.vibrator.IVibrator");
            System.out.println("off transact=" + hal.transact(2, p, r, 0));
            r.readException(); System.out.println("off status=ok"); return;
        }
        IBinder ext = (IBinder) IBinder.class.getMethod("getExtension").invoke(hal);
        System.out.println("extension: " + (ext == null ? null : ext.getInterfaceDescriptor()));
        String[] f = a[3].split(",");
        int[] d = new int[f.length];
        for (int i = 0; i < f.length; i++) d[i] = Integer.parseInt(f[i].trim());
        d[0] = d.length;
        Parcel p = Parcel.obtain(ext), r = Parcel.obtain();
        try {
            p.writeInterfaceToken(DESC);
            p.writeIntArray(d);
            p.writeInt(Integer.parseInt(a[0]));
            p.writeLong(Long.parseLong(a[1]));
            p.writeInt(Integer.parseInt(a[2]));
            boolean ok = ext.transact(1, p, r, 0);
            r.readException();
            System.out.println("play(" + d.length + " ints) transact=" + ok + " status=ok");
        } catch (Exception e) {
            System.out.println("play failed: " + e);
        } finally { p.recycle(); r.recycle(); }
    }
}
