# Lora_FreeRtos_Test


That program manages the packages received from I2C, two Lora modules working at 915MHz, and one Lora module working at 2.4GHz by using FreeRtos. 
The tasks are organized to do:

1) Any data received from I2C will be placed into a queue for the Lora@915MHz.
2) One module Lora@915MHz will receive any package, and it will be placed into the queue for sending back throughout Lora@915MHz and Lora@2.4GHz.

The library for the Lora modules was not included. 
