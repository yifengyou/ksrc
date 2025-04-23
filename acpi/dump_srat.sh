#!/bin/bash


apt-get install acpidump
acpidump > acpi.dat
acpixtract -s SRAT acpi.dat
iasl -d srat.dat


