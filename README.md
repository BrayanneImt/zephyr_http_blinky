# Guide de compilation de l'application et deploiement sur un equipement IoT

## Préréquis

- Installer `Zephyr Os`

## Cloner le repository

```bash
git clone https://github.com/BrayanneImt/zephyr_http_blinky.git
```

## Copie du projet dans le repertoire d'installation de Zephyr Os(zephyrproject:exemple)

```bash
# copie avec les permissions
sudo cp -a ~/Download/zephyr_http_blinky ~/zephyrproject/
```

## Compilation du projet et deploiement sur un equipement IoT

```bash
# Activer l'environnement Zephyr
source ~/zephyrproject/.venv/bin/activate
source ~/zephyrproject/zephyr/zephyr-env.sh

# Se placer dans le projet
cd ~/zephyrproject/zephyr_http_blinky

# Compiler pour l'equipement IoT (heltec_wifi_lora32_v3/esp32s3/procpu est le support de notre equipement iot sur zephyr)
west build -p always -b heltec_wifi_lora32_v3/esp32s3/procpu .

# Flasher (brancher le Heltec en USB d'abord)
west flash

# Surveiller les logs série
west espressif monitor
```
