// pic_real: 3M iter, array of 3 shapes.
public class Main {
    static abstract class Shape { abstract int area(); }
    static class Circle extends Shape { int r; Circle(int x) { r = x; } int area() { return r*r*3; } }
    static class Rect extends Shape { int w, h; Rect(int x, int y) { w = x; h = y; } int area() { return w*h; } }
    static class Triangle extends Shape { int b, h; Triangle(int x, int y) { b = x; h = y; } int area() { return (b*h)/2; } }
    public static void main(String[] args) {
        Shape[] shapes = new Shape[3];
        shapes[0] = new Circle(5);
        shapes[1] = new Rect(4, 6);
        shapes[2] = new Triangle(3, 8);
        int sum = 0; int i = 0;
        while (i < 3000000) {
            sum += shapes[i % 3].area();
            i++;
        }
        System.exit(sum & 0xFF);
    }
}
