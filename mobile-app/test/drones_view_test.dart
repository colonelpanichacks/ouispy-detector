import 'package:flutter_test/flutter_test.dart';
import 'package:ouispy_mesh/views/drones_view.dart';

void main() {
  test('the drone card ages stay short', () {
    const now = 1.0e9;
    expect(shortAge(0, now), '-');
    expect(shortAge(now - 2, now), 'now');
    expect(shortAge(now - 42, now), '42s');
    expect(shortAge(now - 600, now), '10m');
    expect(shortAge(now - 3 * 3600 - 1200, now), '3h');
    expect(shortAge(now - 86400 - 5, now), '1d');
    expect(shortAge(now - 30 * 86400, now), '4w');
  });
}
