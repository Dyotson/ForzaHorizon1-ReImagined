"""Compose the licensed Gamepad Viewer sprites; keep the upstream artwork intact."""
from pathlib import Path

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

ASSETS = Path(__file__).with_name('controller_art')

# Bounds in the upstream artwork's coordinates, before the optional crop.
REGIONS = {
    'xbox': dict(left_trigger=(151, 0, 89, 122), right_trigger=(510, 0, 89, 122),
                 left_shoulder=(107, 129, 170, 61), right_shoulder=(473, 129, 170, 61),
                 left_stick=(135, 229, 103, 103), right_stick=(422, 342, 103, 103),
                 dpad=(223, 345, 110, 111), back=(302, 260, 40, 40), start=(410, 260, 40, 40),
                 a=(540, 303, 53, 53), b=(590, 253, 53, 53), x=(490, 253, 53, 53), y=(540, 202, 53, 53)),
    'dualshock': dict(left_trigger=(109, 0, 99, 90), right_trigger=(598, 0, 99, 90),
                      left_shoulder=(109, 94, 99, 23), right_shoulder=(598, 94, 99, 23),
                      left_stick=(228, 308, 94, 94), right_stick=(496, 308, 94, 94),
                      dpad=(92, 181, 125, 126), back=(227, 142, 28, 46), start=(551, 142, 28, 46),
                      a=(623, 273, 56, 56), b=(681, 215, 56, 56), x=(567, 215, 56, 56), y=(623, 159, 56, 56)),
    'dualsense': dict(left_trigger=(299, 38, 99, 90), right_trigger=(798, 38, 99, 90),
                      left_shoulder=(263, 132, 147, 34), right_shoulder=(788, 132, 147, 34),
                      left_stick=(422, 348, 94, 94), right_stick=(681, 348, 94, 94),
                      dpad=(286, 220, 125, 126), back=(397, 170, 26, 43), start=(773, 170, 26, 43),
                      a=(820, 316, 55, 55), b=(880, 257, 55, 55), x=(762, 257, 55, 55), y=(820, 197, 55, 55)),
}


def load_image(path):
    if path.suffix == '.svg':
        svg = QSvgRenderer(str(path))
        if not svg.isValid():
            raise ValueError(f'Controller artwork could not be loaded: {path.name}')
        pixmap = QPixmap(svg.defaultSize())
        pixmap.fill(Qt.transparent)
        painter = QPainter(pixmap)
        svg.render(painter)
        painter.end()
    else:
        pixmap = QPixmap(str(path))
    if pixmap.isNull():
        raise ValueError(f'Controller artwork could not be loaded: {path.name}')
    return pixmap


def compose(family):
    directory = ASSETS / {'xbox': 'xbox-one', 'dualshock': 'ds4', 'dualsense': 'dualsense'}[family]
    suffix = '.png' if family == 'dualsense' else '.svg'
    canvas = load_image(directory / ('base-white' + suffix))
    painter = QPainter(canvas)

    def sprite(name, source, destination):
        image = load_image(directory / (name + suffix))
        painter.drawPixmap(QRectF(*destination), image, QRectF(*source))

    try:
        if family == 'xbox':
            for key, column in (('a', 0), ('b', 53), ('x', 106), ('y', 159)):
                sprite('buttons', (column, 0, 53, 53), REGIONS[family][key])
            for destination in ((144, 239, 83, 83), (432, 352, 83, 83)):
                sprite('stick', (85, 0, 83, 83), destination)
        elif family == 'dualshock':
            for key, column in (('a', 0), ('b', 56), ('x', 112), ('y', 168)):
                sprite('buttons', (column, 0, 56, 56), REGIONS[family][key])
            for key in ('left_stick', 'right_stick'):
                sprite('sticks', (0, 0, 94, 94), REGIONS[family][key])
            for source, destination in (
                    ((37, 0, 36, 52), (136, 181, 36, 52)),
                    ((0, 0, 36, 52), (136, 255, 36, 52)),
                    ((74.5, 0, 52, 36), (92, 225, 52, 36)),
                    ((126.5, 0, 52, 36), (165, 225, 52, 36))):
                sprite('dpad', source, destination)
        else:
            for key, column in (('a', 0), ('b', 57), ('x', 113), ('y', 169)):
                sprite('buttons', (column, 0, 55, 55), REGIONS[family][key])
            for source, destination in (
                    ((37, 0, 37, 52), (330, 220, 37, 52)),
                    ((0, 0, 37, 52), (330, 294, 37, 52)),
                    ((75, 0, 52, 40), (286, 265, 52, 40)),
                    ((127, 0, 52, 40), (359, 265, 52, 40))):
                sprite('dpad', source, destination)
    finally:
        painter.end()
    crop = (200, 30, 800, 650) if family == 'dualsense' else (0, 0, canvas.width(), canvas.height())
    regions = {key: QRectF(x-crop[0], y-crop[1], w, h) for key, (x, y, w, h) in REGIONS[family].items()}
    return canvas.copy(*crop), regions
