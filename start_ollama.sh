#!/bin/bash
sudo snap run --shell ollama
export OLLAMA_USE_GPU=1
ollama serve
ollama pull gemma3:4b

