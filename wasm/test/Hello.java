import java.util.*;
import java.util.stream.*;

public class Hello {
    public static void main(String[] args) throws Exception {
        System.out.println("Hello from HotSpot Zero on WebAssembly!");
        System.out.println("java.vm.name = " + System.getProperty("java.vm.name"));
        System.out.println("os.arch      = " + System.getProperty("os.arch"));
        System.out.println("cpus         = " + Runtime.getRuntime().availableProcessors());

        // collections + streams + lambdas (invokedynamic)
        List<Integer> squares = IntStream.rangeClosed(1, 10).map(i -> i * i).boxed().collect(Collectors.toList());
        System.out.println("squares      = " + squares);

        // long / double arithmetic
        long f = 1; for (int i = 2; i <= 20; i++) f *= i;
        System.out.println("20!          = " + f + ", sqrt(2) = " + Math.sqrt(2.0));

        // threads
        int[] sum = new int[1];
        Thread[] ts = new Thread[4];
        for (int t = 0; t < ts.length; t++) {
            final int id = t;
            ts[t] = new Thread(() -> { synchronized (sum) { sum[0] += id + 1; } }, "worker-" + t);
            ts[t].start();
        }
        for (Thread t : ts) t.join();
        System.out.println("thread sum   = " + sum[0] + " (expect 10)");

        // exceptions
        try { Object o = null; o.hashCode(); }
        catch (NullPointerException e) { System.out.println("caught NPE   = " + e.getMessage()); }

        // GC
        List<byte[]> junk = new ArrayList<>();
        for (int i = 0; i < 200; i++) { junk.add(new byte[256 * 1024]); if (junk.size() > 20) junk.clear(); }
        System.gc();
        System.out.println("gc ok, free  = " + Runtime.getRuntime().freeMemory() / 1024 + " KB");
    }
}
