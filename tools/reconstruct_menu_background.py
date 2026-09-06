from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
EXTRACTED = ROOT / "game-files" / "HD_UI" / "Extracted"
SOURCE = EXTRACTED / "main_menu_source_640x480.png"
OUTPAINT_TARGET = EXTRACTED / "main_menu_outpaint_target_854x480.png"
LEFT_TARGET = EXTRACTED / "main_menu_left_outpaint_target_320x480.png"
RIGHT_TARGET = EXTRACTED / "main_menu_right_outpaint_target_320x480.png"

TILE_POSITIONS = (
    (0, 0),
    (256, 0),
    (448, 0),
    (0, 240),
    (256, 240),
    (448, 240),
)


def main() -> None:
    canvas = Image.new("RGBA", (704, 496))
    for index, position in enumerate(TILE_POSITIONS):
        tile = Image.open(EXTRACTED / f"main_menu_tile_{index}.bmp").convert("RGBA")
        canvas.alpha_composite(tile, position)

    source = canvas.crop((0, 0, 640, 480))
    source.save(SOURCE)

    target = Image.new("RGBA", (854, 480), (0, 0, 0, 0))
    target.alpha_composite(source, (107, 0))
    target.save(OUTPAINT_TARGET)

    left_target = Image.new("RGBA", (320, 480), (0, 0, 0, 0))
    left_target.alpha_composite(source.crop((0, 0, 213, 480)), (107, 0))
    left_target.save(LEFT_TARGET)

    right_target = Image.new("RGBA", (320, 480), (0, 0, 0, 0))
    right_target.alpha_composite(source.crop((427, 0, 640, 480)), (0, 0))
    right_target.save(RIGHT_TARGET)

    print(SOURCE)
    print(OUTPAINT_TARGET)
    print(LEFT_TARGET)
    print(RIGHT_TARGET)


if __name__ == "__main__":
    main()
