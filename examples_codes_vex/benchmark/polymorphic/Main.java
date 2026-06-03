// polymorphic: 10M, 3 subtypes.
public class Main {
    static abstract class Shape { abstract int area(); }
    static class Circle extends Shape { int r; Circle(int x) { r = x; } int area() { return r*r*3; } }
    static class Rect extends Shape { int w, h; Rect(int x, int y) { w = x; h = y; } int area() { return w*h; } }
    static class Triangle extends Shape { int b, h; Triangle(int x, int y) { b = x; h = y; } int area() { return (b*h)/2; } }
    public static void main(String[] args) {
        Shape c = new Circle(5);
        Shape r = new Rect(4, 6);
        Shape t = new Triangle(3, 8);
        int sum = 0; int i = 0;
        while (i < 10000000) {
            int m = i % 3;
            if (m == 0) sum += c.area();
            else if (m == 1) sum += r.area();
            else sum += t.area();
            i++;
        }
        System.exit(sum & 0xFF);
    }
}
