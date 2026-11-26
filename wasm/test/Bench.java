/*
 * Small CPU benchmark for comparing interpreters (wasm Zero, native Zero,
 * native -Xint). Each kernel prints "<name> <ms> <checksum>"; the checksums
 * must agree across VMs. Optional argument: size factor (default 1).
 */
public class Bench {
    interface Shape { int area(); }
    static final class Sq implements Shape { final int s; Sq(int s) { this.s = s; } public int area() { return s * s; } }
    static final class Rect implements Shape { final int w, h; Rect(int w, int h) { this.w = w; this.h = h; } public int area() { return w * h; } }
    static final class Counter { int a; long b; double c; }
    static final class Node { final int v; final Node next; Node(int v, Node n) { this.v = v; this.next = n; } }

    static long intLoop(int n) {
        long s = 0;
        for (int i = 0; i < n; i++) s += (i * i) % 7;
        return s;
    }
    static long fields(int n) {
        Counter c = new Counter();
        for (int i = 0; i < n; i++) { c.a += i; c.b += c.a; c.c += 0.5; }
        return c.a + c.b + (long) c.c;
    }
    static long calls(int n) {
        Shape[] shapes = { new Sq(3), new Rect(2, 5), new Sq(4), new Rect(1, 7) };
        long s = 0;
        for (int i = 0; i < n; i++) s += shapes[i & 3].area();
        return s;
    }
    static int sieve(int n, int reps) {
        int count = 0;
        for (int r = 0; r < reps; r++) {
            boolean[] composite = new boolean[n + 1];
            count = 0;
            for (int i = 2; i <= n; i++) {
                if (!composite[i]) { count++; for (int j = i + i; j <= n; j += i) composite[j] = true; }
            }
        }
        return count;
    }
    static long alloc(int n) {
        long s = 0;
        Node list = null;
        for (int i = 0; i < n; i++) {
            list = new Node(i, list);
            if ((i & 1023) == 0) { for (Node p = list; p != null; p = p.next) s += p.v; list = null; }
        }
        return s;
    }
    static long longDouble(int n) {
        long l = 1; double d = 1.0;
        for (int i = 1; i < n; i++) { l = l * 31 + i; d = d * 1.0000001 + i * 0.5; }
        return l ^ Double.doubleToLongBits(d);
    }
    static long strings(int n) {
        long h = 0;
        for (int i = 0; i < n; i++) {
            StringBuilder sb = new StringBuilder();
            sb.append("item-").append(i).append('/').append(i * 3);
            h = h * 31 + sb.toString().hashCode();
        }
        return h;
    }

    static long total;
    static void run(String name, java.util.function.LongSupplier k) {
        long t = System.nanoTime();
        long r = k.getAsLong();
        long ms = (System.nanoTime() - t) / 1_000_000;
        total += ms;
        System.out.println(name + " " + ms + " " + r);
    }

    public static void main(String[] args) {
        int f = args.length > 0 ? Integer.parseInt(args[0]) : 1;
        run("intLoop", () -> intLoop(2_000_000 * f));
        run("fields", () -> fields(1_000_000 * f));
        run("calls", () -> calls(1_000_000 * f));
        run("sieve", () -> sieve(100_000, 5 * f));
        run("alloc", () -> alloc(500_000 * f));
        run("longDouble", () -> longDouble(1_000_000 * f));
        run("strings", () -> strings(50_000 * f));
        System.out.println("total " + total + " -");
    }
}
