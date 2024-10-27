# Install FreeRTOS for PI Pico

* [Install SDK](https://www.raspberrypi.com/news/raspberry-pi-pico-windows-installer/)

# Add FreeRTOS as submodule

## There are two options (at least)

I am unsure what the difference is, but have chosen option 2

### 1 - Use FreeRTOS source code from FreeRTOS home 

[FreeRTOS GitHub repo](https://github.com/FreeRTOS/FreeRTOS-Kernel)

```text
cd external
git submodule add https://github.com/FreeRTOS/FreeRTOS-Kernel.git 
```

### 2- Use FreeRTOS source code from Raspberry Pi's GitHub repo. 

[Raspberry Pi GitHub repo](https://github.com/raspberrypi/FreeRTOS-kernel)

```text
cd external
git submodule add https://github.com/raspberrypi/FreeRTOS-Kernel.git
```

## Init the submodules

```text
git submodule update --init --recursive
```