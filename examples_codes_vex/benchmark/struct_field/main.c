// struct_field: 30M field read/write.
typedef struct { int x; int y; int z; } Vec3;
int main(void) {
    Vec3 v = {1, 2, 3};
    int sum = 0; int i = 0;
    while (i < 30000000) {
        v.x += 1; v.y += 2; v.z += 3;
        sum += v.x + v.y + v.z;
        i++;
    }
    return sum;
}
