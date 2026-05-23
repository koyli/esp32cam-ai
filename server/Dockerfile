FROM python:3.11-slim
RUN pip install flask
WORKDIR /app
COPY upload_server.py .
CMD ["python3", "upload_server.py"]
