/*
 * Correctness test for the experimental wasm32 baseline compiler (WASM_JIT=1).
 * Every kernel runs many times (so it gets compiled) and the checksum of all
 * results is printed; it must be identical with and without the compiler.
 */
public class JitTest {
    static int sf; static long sl; static double sd; static Object so;
    int f; long l; float fl; double d; byte b; char c; short s; boolean z; Object o;
    static volatile int vi; static volatile long vl;

    static int loop(int n) { int s = 0; for (int i = 0; i < n; i++) s += i * i % 7; return s; }
    static long longs(int n) { long a = 1, b = 0; for (int i = 0; i < n; i++) { long t = a * 31 + b; b = a ^ (t >>> 3); a = t << 1 | (t >>> 63); } return a + b; }
    static double doubles(int n) { double x = 0.5; float y = 1.5f; for (int i = 0; i < n; i++) { x = x * 1.0001 + i / 3.0; y = y * 0.999f + (float) x; } return x + y + (int) x + (long) y; }
    static int divs(int a, int b) { return a / b + a % b; }                      // b == 0 -> bailout -> exception
    static long ldivs(long a, long b) { return a / b - a % b; }
    static int minDiv() { int m = Integer.MIN_VALUE; int r = 0; r += m / -1; r += m % -1; return r; }
    static int cmps(double a, double b, float c, float d) {
        int r = 0;
        if (a < b) r += 1; if (a > b) r += 2; if (a == b) r += 4; if (c < d) r += 8; if (c >= d) r += 16;
        if (Double.isNaN(a) ? a != a : true) r += 32;
        return r + Double.compare(a, b) * 64 + Long.compare((long) a, (long) b) * 128;
    }
    static int convs(double v) { return (int) v + (int) (float) v + (byte) v + (char) v + (short) v + (int) (long) v; }
    int fields(int n) { for (int i = 0; i < n; i++) { f += i; l += f; fl += 0.5f; d += fl; b = (byte) (b + 3); c = (char) (c + 7); s = (short) (s - 5); z = !z; } return f + (int) l + (int) fl + (int) d + b + c + s + (z ? 1 : 0); }
    static int statics(int n) { for (int i = 0; i < n; i++) { sf += i; sl += sf; sd += sl * 0.25; vi += 1; vl += vi; } return sf + (int) sl + (int) sd + vi + (int) vl; }
    static Object refs(JitTest t, Object x) { t.o = x; so = t.o; return so == x ? t : null; }
    static int arrays(int n) {
        int[] ia = new int[n]; long[] la = new long[n]; byte[] ba = new byte[n]; char[] ca = new char[n]; short[] sa = new short[n]; double[] da = new double[n]; boolean[] za = new boolean[n];
        int r = 0; return fillArrays(ia, la, ba, ca, sa, da, za);
    }
    static int fillArrays(int[] ia, long[] la, byte[] ba, char[] ca, short[] sa, double[] da, boolean[] za) {
        int n = ia.length; int r = 0;
        for (int i = 0; i < n; i++) { ia[i] = i * 3; la[i] = (long) i << 33; ba[i] = (byte) i; ca[i] = (char) (i * 1000); sa[i] = (short) (i * -77); da[i] = i * 0.1; za[i] = (i & 1) == 0; }
        for (int i = 0; i < n; i++) r += ia[i] + (int) (la[i] >>> 30) + ba[i] + ca[i] + sa[i] + (int) da[i] + (za[i] ? 1 : 0);
        return r;
    }
    static int bounds(int[] a, int i) { return a[i]; }                       // out of bounds -> bailout -> exception
    static int nulls(JitTest t) { return t.f; }                               // null -> bailout -> NPE
    static String unsupported(int i) { return "v" + i; }                      // invokedynamic etc. -> bailout
    static int stackOps(int a, int b) { int[] x = { a }; x[0]++; long q = a; q = q + b; int y = (x[0] = b) + a; return y + (int) q + x[0]; }
    static int switches(int k) { switch (k & 3) { case 0: return 10; case 1: return 20; default: return 30; } }  // tableswitch -> bailout

    public static void main(String[] args) {
        int reps = args.length > 0 ? Integer.parseInt(args[0]) : 300;
        long h = 0;
        JitTest t = new JitTest();
        int[] small = new int[4];
        for (int r = 0; r < reps; r++) {
            h = h * 31 + loop(200);
            h = h * 31 + longs(100);
            h = h * 31 + Double.doubleToLongBits(doubles(50));
            h = h * 31 + divs(r + 7, (r % 5) + 1) + ldivs(r * 1000L, 7) + minDiv();
            h = h * 31 + cmps(r * 0.5, 7.0, r, 3f) + cmps(Double.NaN, 1.0, Float.NaN, 0f);
            h = h * 31 + convs(r * 1234.567 - 50000) + convs(Double.NaN) + convs(1e30);
            h = h * 31 + t.fields(20) + statics(10);
            h = h * 31 + (refs(t, "x") == t ? 1 : 0);
            h = h * 31 + arrays(30);
            h = h * 31 + stackOps(r, 5) + switches(r) + unsupported(r).length();
            try { divs(1, 0); } catch (ArithmeticException e) { h += 7; }
            try { bounds(small, 4 + (r & 1)); } catch (ArrayIndexOutOfBoundsException e) { h += 11; }
            try { nulls(null); } catch (NullPointerException e) { h += 13; }
            h += bounds(small, r & 3);
        }
        System.out.println("checksum " + h);
    }
}
