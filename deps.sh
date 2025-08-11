echo "This code is tested under Ubuntu 22.02.0, not 24"
sudo apt-get -y update
sudo apt install -y g++
sudo apt install -y libserialport-dev
sudo apt install -y make
sudo apt install -y libjsoncpp-dev
sudo apt install -y libreadline-dev
sudo apt install -y pkg-config libcurl4-openssl-dev
sudo apt install -y libpoppler-cpp-dev libtesseract-dev libleptonica-dev tesseract-ocr
sudo snap install ollama
sudo snap start ollama
ollama pull gemma3:4b
ollama pull gemma3:4b-it-qat
ollama pull nomic-embed-text
