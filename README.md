# `zephyr-example-setup`

> Example setup of applications using the Auxspace Zephyr port

## General Info

### Technical Terms

* `RTOS`: **R**eal-**T**ime **O**perating **S**ystem
* `µC`: **Mi**cro**C**ontroller (µ - Controller)
* `MCU`: **M**icro**C**ontroller **Unit**
* `SOC`: **S**ystem **O**n **C**hip
* `Zephyr`: Open-Source RTOS Kernel for a large variety of MCUs and SOCs
* `fork`: A *forked* repository is a direct clone of another, with custom
modifications made
* `Docker`: "Docker Engine is an open source containerization technology
for building and containerizing your applications"
(<https://docs.docker.com/engine/>)
* `device-tree`: Tree-like configuration for hardware integration

### Project Information

This Project is a fork of the official
[Zephyr Example Project](https://github.com/zephyrproject-rtos/example-application)
with a few tweaks.
All changes made in comparison to the zephyr example application are
traceable through the well-documented git history of this project.
The original README.md from the upstream repository has been moved to
[doc/README.md](./doc/README.md) for the sole purpose of having an
easier and more specific entrypoint for newcomers and first semester
students with this Auxspace-specific document.

As the title of this repository suggests, the application is using zephyr
as an RTOS.
The Zephyr Kernel acts **application-centric**, which means that
the application implements the main entrypoint and includes the
Zephyr Kernel.
Zephyr supports memory protection, simultaneous multi-processing
(`SMP`) and many more useful features.
Integrating custom boards is kept straighforward with
device-tree and Kconfig hardware configuration, similar
to the [Linux Kernel](https://github.com/torvalds/linux).

## Setup

Setup Zephyr using either the docker container in this directory or follow the
[Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
from the Zephyr project.

### SSH Keys for GitHub

To clone the sources, you also have to have an SSH keypair, which is connected to
your github account.

Create a keypair like this:

```bash
ssh-keygen -t rsa
```

This command creates two files (if run exacly as shown above):

* `$HOME/.ssh/id_rsa` (Private Key)
* `$HOME/.ssh/id_rsa.pub` (Public Key)

The private key is to be kept secret from everyone, while the
public key can be shared.
Next, head to Github and into your
[profile settings](https://github.com/settings/profile)
and choose [`SSH and GPG Keys`](https://github.com/settings/keys)
in the menu on the left side.
Use the `New SSH Key` button to add your public key and click `save`.

Done! You now have linked your ssh key to your GitHub profile and
can continue with the next steps.

### Docker Container and Zephyr Workspace

```bash
# Open a shell in the development container
./run.sh --ssh-key PUT_SSH_PRIVATE_KEY shell

# Install this example repo into the container
# use "/workdir/zephyr-workspace" instead of "zephyr-workspace"
# to access it from outside the container as well
west init -m git@github.com:AUXSPACEeV/zephyr-example-setup --mr main zephyr-workspace
cd zephyr-workspace
west update

# Python requirements
pip3 install -r ./zephyr/scripts/requirements.txt
```

## Build

```bash
# use "/workdir/zephyr-workspace" instead of "~/zephyr-workspace"
# if you used "/workdir/zephyr-workspace" in setup
cd ~/zephyr-workspace/zephyr-example-setup

# Build the project for the m33 core on the rpi pico 2 (example)
west build -b rpi_pico2/rp2350a/m33 app
```

The output from the build will be at `zephyr-workspace/zephyr-example-setup/build/zephyr`
called `zephyr.uf2`.

## Deployment

Deploy the binary to your board by either using `west flash` or in case of the
Raspberry Pi Pico series, copy it onto the Pi's storage.

