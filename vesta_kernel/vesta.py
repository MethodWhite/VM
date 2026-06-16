import sys
import os

# Add src to path for internal imports
sys.path.append(os.path.join(os.path.dirname(__file__), 'src'))

from src.cli.main import main

if __name__ == "__main__":
    main()
