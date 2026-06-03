// struct_field: 30M field read/write.
public class Main {
    static class Vec3 { int x = 1; int y = 2; int z = 3; }
    public static void main(String[] args) {
        Vec3 v = new Vec3();
        int sum = 0; int i = 0;
        while (i < 30000000) {
            v.x += 1; v.y += 2; v.z += 3;
            sum += v.x + v.y + v.z;
            i++;
        }
        System.exit(sum & 0xFF);
    }
}
