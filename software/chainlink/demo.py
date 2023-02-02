import random
import time
from datetime import datetime

from splitflap_proto import (
    ask_for_serial_port,
    splitflap_context,
)

words = [
    '    ', 'ty  ', ' ler', 'is  ', 'bad ', 'at  ', 'sma ', '  sh'
]


def _run():
    p = ask_for_serial_port()
    with splitflap_context(p) as s:
        modules = s.get_num_modules()
        alphabet = s.get_alphabet()

        # Show a random word every 10 seconds
        now = datetime.now()

        print_time = now.strftime("%H%M")
        while True:
            now = datetime.now()

            current_time = now.strftime("%H%M")
            if current_time != print_time:
                print_time = current_time
                s.set_text(print_time)
            time.sleep(1)


if __name__ == '__main__':
    _run()
