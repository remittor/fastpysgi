sudo rm -rf /usr/local/lib/python3.8/dist-packages/_fastpysgi*;
sudo rm -rf /usr/local/lib/python3.8/dist-packages/fastpysgi*;
rm -rf build/ dist/ bin/ __pycache__/
sudo CC="ccache clang-11" python3 setup.py install