"""Configure Linux graphics without replacing unrelated settings. Restart to apply."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
SETTINGS = ROOT / 'out/build/linux-release/settings.toml'


def update(text, values):
    # Our scalar keys live in the root table, before any [section].
    split = re.search(r'^\s*\[', text, re.M)
    root, tail = (text[:split.start()], text[split.start():]) if split else (text, '')
    for key, value in values.items():
        encoded = json.dumps(value) if isinstance(value, str) else str(value).lower()
        line = f'{key} = {encoded}'
        pattern = rf'^[ \t]*{re.escape(key)}[ \t]*=.*$'
        if re.search(pattern, root, re.M):
            root = re.sub(pattern, line, root, flags=re.M)
        else:
            root = root.rstrip() + '\n' + line + '\n'
    return root.rstrip() + '\n' + tail


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--motion-blur', choices=['on', 'off'])
    parser.add_argument('--light-streaks', choices=['on', 'off'])
    parser.add_argument('--camera-orbit', choices=['on', 'off'])
    parser.add_argument('--road-shine', choices=['on', 'off'])
    parser.add_argument('--renderer', choices=['opengl', 'vulkan'],
                        help='native Vulkan 1.3 or OpenGL 4.5')
    parser.add_argument('--vsync', choices=['on', 'off'])
    parser.add_argument('--fps', type=int, choices=[0, 30, 60, 90, 120, 144, 165, 240])
    parser.add_argument('--resolution', choices=['1280x720', '1920x1080', '2560x1440', '3840x2160'])
    parser.add_argument('--render-height', type=int, choices=[480, 720, 1080, 1440, 2160])
    parser.add_argument('--show', action='store_true')
    args = parser.parse_args()
    text = SETTINGS.read_text() if SETTINGS.exists() else ''
    values = {}
    if args.renderer is not None:
        values['linux_renderer'] = args.renderer
    for key in ['motion_blur', 'light_streaks', 'road_shine', 'vsync', 'camera_orbit']:
        v = getattr(args, key)
        if v is not None:
            values[key] = v == 'on'
    if args.fps is not None:
        values['fps_cap'] = args.fps
    if args.render_height is not None:
        values['render_height'] = args.render_height
    if args.resolution:
        values['width'], values['height'] = map(int, args.resolution.split('x'))
        values.setdefault('render_height', values['height'])
    if not values and not args.show:
        parser.print_help()
        return
    if values:
        SETTINGS.parent.mkdir(parents=True, exist_ok=True)
        new = update(text, values)
        temporary = SETTINGS.with_suffix('.toml.tmp')
        temporary.write_text(new)
        temporary.replace(SETTINGS)
        text = new
        print('Saved. Restart the game to apply these settings.')
    print(f'{SETTINGS}:\n{text or "(defaults: effects on, 30 FPS, native render resolution)"}')


if __name__ == '__main__':
    main()
