import json
import queue
import socket
import sys
import time
import sounddevice as sd
from vosk import KaldiRecognizer, Model

ESP32_IP = "192.168.1.161"  
ESP32_PORT = 8080

MODELO_RUTA = "model_es"
SAMPLE_RATE = 16000
BLOCK_SIZE = 4000          # 0.25 s por bloque
UMBRAL_CONFIANZA = 0.80    # Confianza mínima requerida (80%)
PERSISTENCIA_MINIMA = 1    # Repeticiones consecutivas válidas antes de enviar

GRAMATICA = '["abrir", "cerrar", "alto", "reiniciar", "[unk]"]'
COMANDOS = {
    "abrir": "CMD:OPEN",
    "cerrar": "CMD:CLOSE",
    "alto": "CMD:SAFE",
    "reiniciar": "CMD:RESET",
}

cola_audio: queue.Queue[bytes] = queue.Queue()


def audio_cb(indata, frames, time_info, status) -> None:
    if status:
        print(f"[AUDIO] {status}", file=sys.stderr)
    cola_audio.put(bytes(indata))


def extraer_prediccion(raw_json: str) -> tuple[str | None, float]:
    datos = json.loads(raw_json)
    palabras = datos.get("result", [])
    if not palabras:
        return None, 0.0
    ultima = palabras[-1]
    palabra = ultima.get("word", "").strip().lower()
    confianza = float(ultima.get("conf", 0.0))
    return COMANDOS.get(palabra), confianza


def enviar_linea(sock: socket.socket, mensaje: str) -> None:
    sock.sendall(f"{mensaje}\n".encode("utf-8"))
    print(f"[TX -> ESP32] {mensaje}")


def leer_entradas_esp32(sock: socket.socket) -> None:
    try:
        datos = sock.recv(1024)
        if datos:
            for linea in datos.decode("utf-8", errors="ignore").splitlines():
                if linea.strip():
                    print(f"[RX <- ESP32] {linea.strip()}")
    except BlockingIOError:
        pass


def main() -> None:
    modelo = Model(MODELO_RUTA)
    rec = KaldiRecognizer(modelo, SAMPLE_RATE, GRAMATICA)
    rec.SetWords(True)

    ultimo_cmd: str | None = None
    conteo_persistencia = 0
    ultimo_enviado = ""
    ultimo_ping = time.time()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(3.0)
        sock.connect((ESP32_IP, ESP32_PORT))
        sock.setblocking(False)
        print(f"Conectado a {ESP32_IP}:{ESP32_PORT}. Comandos: abrir, cerrar, alto, reiniciar.")

        with sd.RawInputStream(
            samplerate=SAMPLE_RATE,
            blocksize=BLOCK_SIZE,
            dtype="int16",
            channels=1,
            callback=audio_cb,
        ):
            while True:
                leer_entradas_esp32(sock)

                # Heartbeat cada 1 s para mantener activo el Watchdog con millis() en ESP32
                if time.time() - ultimo_ping >= 1.0:
                    enviar_linea(sock, "CMD:PING")
                    ultimo_ping = time.time()

                try:
                    bloque = cola_audio.get(timeout=0.1)
                except queue.Empty:
                    continue

                estado_texto = rec.Result() if rec.AcceptWaveform(bloque) else rec.PartialResult()
                cmd, conf = extraer_prediccion(estado_texto)

                if cmd is not None and conf >= UMBRAL_CONFIANZA:
                    if cmd == ultimo_cmd:
                        conteo_persistencia += 1
                    else:
                        ultimo_cmd = cmd
                        conteo_persistencia = 1
                    print(
                        f"[IA] {cmd} | conf={conf:.2f} | "
                        f"persistencia={conteo_persistencia}/{PERSISTENCIA_MINIMA}"
                    )
                else:
                    ultimo_cmd = None
                    conteo_persistencia = 0

                if conteo_persistencia >= PERSISTENCIA_MINIMA:
                    if ultimo_cmd != ultimo_enviado or ultimo_cmd in ("CMD:SAFE", "CMD:RESET"):
                        enviar_linea(sock, ultimo_cmd)
                        ultimo_enviado = ultimo_cmd
                    ultimo_cmd = None
                    conteo_persistencia = 0


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nCerrando programa de voz...")
